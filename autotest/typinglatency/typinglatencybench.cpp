/*
 * SPDX-FileCopyrightText: 2026 ghostwriter contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// Keystroke-to-paint latency benchmark.
//
// Drives the real editor stack (MarkdownEditor, MarkdownHighlighter,
// SpellCheckDecorator, DocumentStatistics and OutlineWidget) in a visible
// window. Keystrokes are injected on a fixed schedule, the way the OS would
// queue real input, and each one is timed from its scheduled time until the
// end of the first editor paint that follows it. Any work the event loop is
// busy with when a key "arrives" therefore counts against that key.
//
// Usage: typinglatencybench [--scale N] [--scenario name]

#include <algorithm>

#include <QAbstractEventDispatcher>
#include <QApplication>
#include <QClipboard>
#include <QCommandLineParser>
#include <QDir>
#include <QElapsedTimer>
#include <QMimeData>
#include <QRandomGenerator>
#include <QSettings>
#include <QTest>
#include <QTextBlock>
#include <QTextCursor>
#include <QTimer>

#include <QAccessible>
#include <QRegularExpression>
#include <QSyntaxHighlighter>
#include <QTextBoundaryFinder>

#include <Sonnet/GuessLanguage>
#include <Sonnet/Settings>
#include <Sonnet/Speller>

#include "editor/colorscheme.h"
#include "editor/markdowndocument.h"
#include "editor/markdowneditor.h"
#include "editor/textblockdata.h"
#include "markdown/cmarkgfmapi.h"
#include "outlinewidget.h"
#include "spelling/spellcheckdecorator.h"
#include "statistics/documentstatistics.h"

using namespace ghostwriter;

namespace
{
QElapsedTimer clock_;

qint64 nowUs()
{
    return clock_.nsecsElapsed() / 1000;
}

class TimedEditor : public MarkdownEditor
{
public:
    using MarkdownEditor::MarkdownEditor;

    // End time of the first paint after arm() was called, or -1.
    qint64 firstPaintEndUs = -1;
    qint64 paintTotalUs = 0;
    int paintCount = 0;

    void arm()
    {
        armed = true;
        firstPaintEndUs = -1;
    }

    void paintEvent(QPaintEvent *event) override
    {
        const qint64 start = nowUs();
        MarkdownEditor::paintEvent(event);
        const qint64 end = nowUs();
        paintTotalUs += end - start;
        ++paintCount;

        if (armed) {
            armed = false;
            firstPaintEndUs = end;
        }
    }

private:
    bool armed = false;
};

// Records how long each top-level event dispatch (timer, posted/queued call,
// paint, input) takes.  Input can only be handled between dispatches, so the
// longest one is the worst delay work on the GUI thread can impose on a key.
class LoopMonitor
{
public:
    void start()
    {
        busyChunksUs.clear();
        recording = true;
    }

    void stop()
    {
        recording = false;
    }

    void record(qint64 durationUs)
    {
        if (recording && (durationUs > 50)) {
            busyChunksUs.append(durationUs);
        }
    }

    QList<qint64> busyChunksUs;

private:
    bool recording = false;
};

LoopMonitor *monitorInstance = nullptr;

class BenchApplication : public QApplication
{
public:
    using QApplication::QApplication;

    bool notify(QObject *receiver, QEvent *event) override
    {
        if (depth > 0) {
            return QApplication::notify(receiver, event);
        }

        ++depth;
        const qint64 start = nowUs();
        const bool result = QApplication::notify(receiver, event);

        if (nullptr != monitorInstance) {
            monitorInstance->record(nowUs() - start);
        }

        --depth;
        return result;
    }

private:
    int depth = 0;
};

ColorScheme benchColors()
{
    ColorScheme colors;
    colors.foreground = QColor("#d0d0d0");
    colors.background = QColor("#1e1e1e");
    colors.selection = QColor("#264f78");
    colors.cursor = QColor("#ffffff");
    colors.link = QColor("#569cd6");
    colors.image = QColor("#4ec9b0");
    colors.inlineHtml = QColor("#808080");
    colors.headingText = QColor("#e0e0e0");
    colors.headingMarkup = QColor("#808080");
    colors.emphasisText = QColor("#e0e0e0");
    colors.emphasisMarkup = QColor("#808080");
    colors.blockquoteText = QColor("#b0b0b0");
    colors.blockquoteMarkup = QColor("#808080");
    colors.divider = QColor("#808080");
    colors.listMarkup = QColor("#808080");
    colors.codeText = QColor("#ce9178");
    colors.codeMarkup = QColor("#808080");
    colors.error = QColor("#f44747");
    return colors;
}

const QStringList kWords = QString(
                               "the night came down over the valley and she walked alone toward the "
                               "ruined chapel where the bells had not rung for a hundred years his hand "
                               "found hers in the dark and neither of them spoke of what waited beyond "
                               "the gate a lantern swung somewhere below casting long shadows across "
                               "the wet stones Gareth whispered something Vel pretended not to hear "
                               "blood and ash and old promises were all that remained of the house "
                               "cathedral silence gathered like smoke between the pillars unbroken")
                               .split(' ', Qt::SkipEmptyParts);

// Prose shaped like the user's documents: one heading, paragraphs separated
// by blank lines, mostly 40-400 character lines with occasional very long
// ones, sparse emphasis and dialogue.
QString makeDocument(int paragraphs, quint32 seed)
{
    QRandomGenerator rng(seed);
    QString text = QStringLiteral("# Chapter One\n\n");

    for (int p = 0; p < paragraphs; ++p) {
        int targetLength = 40 + int(rng.bounded(360));

        if (rng.bounded(10) == 0) {
            targetLength = 600 + int(rng.bounded(1400));
        }

        QString paragraph;
        bool sentenceStart = true;

        if (rng.bounded(4) == 0) {
            paragraph += '"';
        }

        while (paragraph.length() < targetLength) {
            QString word = kWords.at(rng.bounded(kWords.size()));

            if (sentenceStart) {
                word[0] = word[0].toUpper();
                sentenceStart = false;
            }

            const int decoration = rng.bounded(40);

            if (decoration == 0) {
                word = '*' + word + '*';
            } else if (decoration == 1) {
                word = "**" + word + "**";
            } else if (decoration == 2) {
                word += "dx"; // Misspelling, so the spell checker has work.
            }

            paragraph += word;

            if (rng.bounded(12) == 0) {
                paragraph += QStringLiteral(". ");
                sentenceStart = true;
            } else if (rng.bounded(9) == 0) {
                paragraph += QStringLiteral(", ");
            } else {
                paragraph += ' ';
            }
        }

        text += paragraph.trimmed() + QStringLiteral(".\n\n");

        if ((p > 0) && (p % 60 == 0)) {
            text += QStringLiteral("## Part %1\n\n").arg(p / 60 + 1);
        }
    }

    return text;
}

// Markdown-heavy text: nested lists, task lists, quotes, fenced code, tables,
// setext headings and emphasis that spans lines.
QString makeRichDocument(int sections, quint32 seed)
{
    QRandomGenerator rng(seed);
    QString text;

    for (int s = 0; s < sections; ++s) {
        text += QStringLiteral("Section %1\n==========\n\n").arg(s);
        text += QStringLiteral("Some *emphasis that\nspans lines* and **strong** text with `code` and a [link](http://x.y).\n\n");
        text += QStringLiteral("- first item\n- second item\n  - nested *item*\n  - another nested\n- [ ] task\n- [x] done task\n\n");
        text += QStringLiteral("1. one\n2. two\n3. three\n\n");
        text += QStringLiteral("> quoted **text**\n> continues here\n\n");
        text += QStringLiteral("```cpp\nint main() {\n    return 0;\n}\n```\n\n");
        text += QStringLiteral("| a | b |\n|---|---|\n| 1 | 2 |\n\n");
        text += QStringLiteral("Subheading\n----------\n\n");

        for (int p = 0; p < 3; ++p) {
            QString paragraph;

            while (paragraph.length() < 80 + int(rng.bounded(200))) {
                paragraph += kWords.at(rng.bounded(kWords.size())) + ' ';
            }

            text += paragraph.trimmed() + QStringLiteral(".\n\n");
        }
    }

    return text;
}

struct KeyAction {
    enum Type {
        Text,
        Key,
        Paste
    } type;
    QChar ch;
    Qt::Key key;
    Qt::KeyboardModifiers modifiers;
    QString pasteText;
};

struct Result {
    QString name;
    QList<qint64> latencyUs;
    QList<qint64> syncUs;
    QList<qint64> busyUs;
    qint64 settleUs = 0;
    int unpainted = 0;
    int paints = 0;
    qint64 paintUs = 0;
};

qint64 percentile(QList<qint64> values, double p)
{
    if (values.isEmpty()) {
        return 0;
    }

    std::sort(values.begin(), values.end());
    int index = qBound(0, int(p * (values.size() - 1) + 0.5), int(values.size() - 1));
    return values.at(index);
}

// Runs a real (blocking) event loop so the dispatcher's awake/aboutToBlock
// signals delimit busy periods.
void pump(int ms)
{
    QEventLoop loop;
    QTimer::singleShot(ms, Qt::PreciseTimer, &loop, &QEventLoop::quit);
    loop.exec();
}

// Waits until the event loop has had no busy chunk longer than 2 ms for
// 1.5 s (i.e., background parse/highlight/spelling/statistics work has
// drained), or until timeout.
qint64 settle(LoopMonitor &monitor, int timeoutMs = 20000)
{
    const qint64 start = nowUs();
    qint64 lastBusy = start;
    monitor.start();

    while ((nowUs() - start) < qint64(timeoutMs) * 1000) {
        const int before = monitor.busyChunksUs.size();
        pump(50);

        for (int i = before; i < monitor.busyChunksUs.size(); ++i) {
            if (monitor.busyChunksUs.at(i) > 2000) {
                lastBusy = nowUs();
            }
        }

        if ((nowUs() - lastBusy) > 1500000) {
            break;
        }
    }

    monitor.stop();
    return lastBusy - start;
}

Result runScenario(const QString &name, TimedEditor &editor, LoopMonitor &monitor, const QList<KeyAction> &actions, int intervalMs)
{
    Result result;
    result.name = name;

    settle(monitor);
    monitor.start();

    const qint64 startUs = nowUs() + 50000;
    const int paintsAtStart = editor.paintCount;
    const qint64 paintTimeAtStart = editor.paintTotalUs;
    int index = 0;
    qint64 pendingScheduledUs = -1;
    QTimer timer;
    timer.setTimerType(Qt::PreciseTimer);
    timer.setSingleShot(true);
    bool done = false;

    QEventLoop loop;

    auto scheduleNext = [&]() {
        if (index >= actions.size()) {
            done = true;
            loop.quit();
            return;
        }

        const qint64 scheduled = startUs + qint64(index) * intervalMs * 1000;
        const qint64 wait = qMax<qint64>(0, (scheduled - nowUs()) / 1000);
        timer.start(int(wait));
    };

    QObject::connect(&timer, &QTimer::timeout, &timer, [&]() {
        // Record the previous key's paint before sending the next key.
        if ((pendingScheduledUs >= 0) && (editor.firstPaintEndUs >= 0)) {
            result.latencyUs.append(editor.firstPaintEndUs - pendingScheduledUs);
        } else if (pendingScheduledUs >= 0) {
            // Nothing was repainted before the next key arrived, i.e., the
            // key had no visible effect (such as Home at column 0).
            result.unpainted++;
        }

        pendingScheduledUs = -1;

        const KeyAction &action = actions.at(index);
        const qint64 scheduled = startUs + qint64(index) * intervalMs * 1000;
        const qint64 before = nowUs();

        editor.arm();

        switch (action.type) {
        case KeyAction::Text:
            QTest::keyClick(&editor, action.ch.toLatin1());
            break;
        case KeyAction::Key:
            QTest::keyClick(&editor, action.key, action.modifiers);
            break;
        case KeyAction::Paste: {
            QMimeData *mime = new QMimeData();
            mime->setText(action.pasteText);
            QApplication::clipboard()->setMimeData(mime);
            QTest::keyClick(&editor, Qt::Key_V, Qt::ControlModifier);
            break;
        }
        }

        result.syncUs.append(nowUs() - before);
        pendingScheduledUs = scheduled;
        ++index;
        scheduleNext();
    });

    scheduleNext();

    if (!done) {
        loop.exec();
    }

    // Wait for the final key's paint.
    const qint64 waitStart = nowUs();

    while ((pendingScheduledUs >= 0) && ((nowUs() - waitStart) < 1000000)) {
        pump(2);

        if (editor.firstPaintEndUs >= 0) {
            result.latencyUs.append(editor.firstPaintEndUs - pendingScheduledUs);
            pendingScheduledUs = -1;
        }
    }

    if (pendingScheduledUs >= 0) {
        result.unpainted++;
    }

    monitor.stop();
    result.busyUs = monitor.busyChunksUs;
    result.paints = editor.paintCount - paintsAtStart;
    result.paintUs = editor.paintTotalUs - paintTimeAtStart;
    result.settleUs = settle(monitor);
    return result;
}

QList<KeyAction> typeText(const QString &text)
{
    QList<KeyAction> actions;

    for (QChar ch : text) {
        if (ch == ' ') {
            actions.append({KeyAction::Key, ch, Qt::Key_Space, Qt::NoModifier, {}});
        } else {
            actions.append({KeyAction::Text, ch, Qt::Key_unknown, Qt::NoModifier, {}});
        }
    }

    return actions;
}

void placeCursorInMiddle(MarkdownEditor &editor, bool endOfBlock)
{
    QTextDocument *doc = editor.document();
    QTextBlock block = doc->findBlockByNumber(doc->blockCount() / 2);

    while (block.isValid() && block.text().trimmed().isEmpty()) {
        block = block.next();
    }

    QTextCursor cursor(block);

    if (endOfBlock) {
        cursor.movePosition(QTextCursor::EndOfBlock);
    } else {
        cursor.movePosition(QTextCursor::Right, QTextCursor::MoveAnchor, qMin(40, block.length() - 1));
    }

    editor.setTextCursor(cursor);
    editor.centerCursor();
}

template<typename F>
double timeMs(int iterations, F &&f)
{
    const qint64 start = nowUs();

    for (int i = 0; i < iterations; ++i) {
        f();
    }

    return (nowUs() - start) / 1000.0 / iterations;
}

void diagnose(TimedEditor &editor, MarkdownDocument *document, const QString &text)
{
    printf("QAccessible::isActive() = %d\n", QAccessible::isActive());

    placeCursorInMiddle(editor, true);
    QTextBlock block = editor.textCursor().block();
    const QString blockText = block.text();
    printf("Target block #%d: %lld chars\n", block.blockNumber(), (long long)blockText.size());

    printf("  parse whole document (cmark + AST clone): %.2f ms\n", timeMs(10, [&]() {
               delete CmarkGfmAPI::instance()->parse(text, false);
           }));

    printf("  rehighlightBlock(target):                 %.2f ms\n", timeMs(20, [&]() {
               editor.highlighter()->rehighlightBlock(block);
           }));

    printf("  relayout target block (markContentsDirty): %.2f ms\n", timeMs(20, [&]() {
               document->markContentsDirty(block.position(), block.length());
           }));

    printf("  full highlighter rehighlight():           %.2f ms\n", timeMs(3, [&]() {
               editor.highlighter()->rehighlight();
           }));

    // Spell checking pieces, the way SpellCheckDecorator does them.
    Sonnet::Speller speller;
    QStringList sentences;
    QTextBoundaryFinder sentenceFinder(QTextBoundaryFinder::Sentence, blockText);
    int prev = 0;

    while (sentenceFinder.toNextBoundary() >= 0) {
        sentences.append(blockText.mid(prev, sentenceFinder.position() - prev));
        prev = sentenceFinder.position();
    }

    QStringList words = blockText.split(QRegularExpression("\\W+"), Qt::SkipEmptyParts);

    printf("  target block: %lld sentences, %lld words\n", (long long)sentences.size(), (long long)words.size());
    printf("  GuessLanguage().identify() per sentence:  %.3f ms  (x%lld sentences = %.2f ms)\n",
           timeMs(5,
                  [&]() {
                      Sonnet::GuessLanguage().identify(sentences.first());
                  }),
           (long long)sentences.size(),
           timeMs(2, [&]() {
               for (const QString &s : sentences) {
                   Sonnet::GuessLanguage().identify(s);
               }
           }));

    Sonnet::GuessLanguage reusedGuesser;
    printf("  GuessLanguage reused, whole block:        %.3f ms -> '%s'\n",
           timeMs(5,
                  [&]() {
                      reusedGuesser.identify(blockText);
                  }),
           qPrintable(reusedGuesser.identify(blockText)));
    printf("  available spelling languages: %s\n", qPrintable(speller.availableLanguages().join(", ")));

    printf("  Speller::isMisspelled() per word:         %.3f ms  (x%lld words = %.2f ms)\n",
           timeMs(1,
                  [&]() {
                      speller.isMisspelled(words.first());
                  }),
           (long long)words.size(),
           timeMs(2, [&]() {
               for (const QString &w : words) {
                   speller.isMisspelled(w);
               }
           }));

    // Break down one real keystroke: time until every contentsChange
    // handler has run versus the total time spent in the key event.
    qint64 changeHandledAt = 0;
    QMetaObject::Connection c = QObject::connect(document, &QTextDocument::contentsChange, document, [&](int, int, int) {
        changeHandledAt = nowUs();
    });

    for (bool spelling : {false, true}) {
        Sonnet::Settings().setCheckerEnabledByDefault(spelling);
        double changeTotal = 0;
        double keyTotal = 0;
        const int n = 10;

        for (int i = 0; i < n; ++i) {
            const qint64 start = nowUs();
            QTest::keyClick(&editor, 'x');
            const qint64 end = nowUs();
            changeTotal += (changeHandledAt - start) / 1000.0;
            keyTotal += (end - start) / 1000.0;
            pump(250);
        }

        printf("  keystroke (spelling %s): insert + contentsChange handlers = %.2f ms, whole key event = %.2f ms\n",
               spelling ? "on " : "off",
               changeTotal / n,
               keyTotal / n);
    }

    QObject::disconnect(c);
}

QString describeFormats(const QList<QTextLayout::FormatRange> &formats)
{
    QStringList parts;

    for (const QTextLayout::FormatRange &range : formats) {
        parts.append(QStringLiteral("[%1+%2 fg=%3 b=%4 i=%5 u=%6 pt=%7]")
                         .arg(range.start)
                         .arg(range.length)
                         .arg(range.format.foreground().color().name())
                         .arg(range.format.fontWeight())
                         .arg(range.format.fontItalic())
                         .arg(int(range.format.underlineStyle()))
                         .arg(range.format.fontPointSize()));
    }

    return parts.join(' ');
}

// Compares the incrementally maintained highlighting against a from-scratch
// parse and full re-highlight.  Returns the number of mismatched blocks.
int verifyHighlighting(TimedEditor &editor, MarkdownDocument *document)
{
    struct Snapshot {
        QList<QTextLayout::FormatRange> formats;
        int state;
    };

    QList<Snapshot> before;

    for (QTextBlock block = document->firstBlock(); block.isValid(); block = block.next()) {
        before.append({block.layout()->formats(), block.userState()});
    }

    int staleSpelling = 0;

#ifndef GW_BENCH_BASELINE
    for (QTextBlock block = document->firstBlock(); block.isValid(); block = block.next()) {
        TextBlockData *data = static_cast<TextBlockData *>(block.userData());

        if ((nullptr == data) || !data->spellCheckValid || (data->spellCheckTextHash != qHash(block.text()))) {
            staleSpelling++;
        }
    }
#endif

    document->setMarkdownAST(CmarkGfmAPI::instance()->parse(document->toPlainText(), false));
    editor.highlighter()->rehighlight();

    int mismatches = 0;
    int index = 0;

    for (QTextBlock block = document->firstBlock(); block.isValid(); block = block.next(), ++index) {
        const Snapshot &snapshot = before.at(index);

        if ((snapshot.formats != block.layout()->formats()) || (snapshot.state != block.userState())) {
            if (mismatches < 8) {
                printf("  MISMATCH block %d state %x vs %x: \"%s\"\n    incremental: %s\n    fresh:       %s\n",
                       index,
                       snapshot.state,
                       block.userState(),
                       qPrintable(block.text().left(60)),
                       qPrintable(describeFormats(snapshot.formats)),
                       qPrintable(describeFormats(block.layout()->formats())));
            }

            mismatches++;
        }
    }

    printf("Verification: %d of %d blocks differ from a fresh highlight; %d blocks with stale spelling results\n", mismatches, index, staleSpelling);
    return mismatches + staleSpelling;
}

void printResult(const Result &r)
{
    auto ms = [](qint64 us) {
        return QString::number(us / 1000.0, 'f', 1);
    };

    qint64 busyTotal = 0;
    int over16 = 0;

    for (qint64 b : r.busyUs) {
        busyTotal += b;
        over16 += (b > 16000) ? 1 : 0;
    }

    printf(
        "%-18s painted=%3lld (+%2d no-op) latency p50=%6s p95=%6s max=%7s ms | in-handler p50=%5s max=%6s ms | "
        "avg paint=%5s ms | stalls>16ms=%2d longest=%6s ms busy=%7s ms | settle=%6s ms\n",
        qPrintable(r.name),
        (long long)r.latencyUs.size(),
        r.unpainted,
        qPrintable(ms(percentile(r.latencyUs, 0.5))),
        qPrintable(ms(percentile(r.latencyUs, 0.95))),
        qPrintable(ms(percentile(r.latencyUs, 1.0))),
        qPrintable(ms(percentile(r.syncUs, 0.5))),
        qPrintable(ms(percentile(r.syncUs, 1.0))),
        qPrintable(ms(r.paints > 0 ? (r.paintUs / r.paints) : 0)),
        over16,
        qPrintable(ms(percentile(r.busyUs, 1.0))),
        qPrintable(ms(busyTotal)),
        qPrintable(ms(r.settleUs)));
    fflush(stdout);
}
}

int main(int argc, char *argv[])
{
    setvbuf(stdout, nullptr, _IONBF, 0);

    // Keep Sonnet's settings writes away from the user's real configuration.
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, QDir::tempPath() + QStringLiteral("/ghostwriter-bench-settings"));

    BenchApplication app(argc, argv);
    QApplication::setOrganizationDomain("kde.org");
    QApplication::setApplicationName("ghostwriter-bench");
    clock_.start();

    QCommandLineParser parser;
    parser.addOption({"scale", "Document size multiplier (1 = ~45 KB).", "n", "1"});
    parser.addOption({"scenario", "Run only the named scenario.", "name"});
    parser.addOption({"no-spelling", "Disable live spell checking."});
    parser.addOption({"interval", "Milliseconds between typed keys.", "ms", "90"});
    parser.addOption({"diagnose", "Time individual components instead of scenarios."});
    parser.addOption({"rich", "Use a Markdown-heavy document instead of prose."});
    parser.process(app);

    const int scale = qMax(1, parser.value("scale").toInt());
    const int interval = qMax(10, parser.value("interval").toInt());
    const QString only = parser.value("scenario");

    Sonnet::Settings sonnetSettings;
    sonnetSettings.setCheckerEnabledByDefault(!parser.isSet("no-spelling"));
    sonnetSettings.save();

    printf("Sonnet: checkerEnabledByDefault=%d (default %d) autodetectLanguage=%d (default %d) language=%s\n",
           sonnetSettings.checkerEnabledByDefault(),
           Sonnet::Settings::defaultCheckerEnabledByDefault(),
           sonnetSettings.autodetectLanguage(),
           Sonnet::Settings::defaultAutodetectLanguage(),
           qPrintable(sonnetSettings.defaultLanguage()));

    MarkdownDocument *document = new MarkdownDocument();
    TimedEditor editor(document, benchColors());
    editor.setAttribute(Qt::WA_ShowWithoutActivating);
    editor.setFont("Consolas", 18);
    editor.setEnableLargeHeadingSizes(true);
    editor.setEditorWidth(EditorWidthMedium);
    editor.resize(1400, 1000);
    editor.move(40, 40);

    SpellCheckDecorator spelling(&editor);
    spelling.setErrorColor(QColor("#f44747"));
    DocumentStatistics statistics(document);
    OutlineWidget outline(&editor);

    LoopMonitor monitor;
    monitorInstance = &monitor;

    editor.show();

    if (!QTest::qWaitForWindowExposed(&editor)) {
        fprintf(stderr, "Window was not exposed\n");
        return 1;
    }

    editor.setupPaperMargins();

    const QString text = parser.isSet("rich") ? makeRichDocument(12 * scale, 1234) : makeDocument(200 * scale, 1234);

    const qint64 loadStart = nowUs();
    editor.setPlainText(text);
    const qint64 loadSync = nowUs() - loadStart;
    const qint64 loadSettle = settle(monitor);

    printf("Document: %lld chars, %d blocks. setPlainText=%.1f ms, background settle=%.1f ms\n",
           (long long)text.size(),
           document->blockCount(),
           loadSync / 1000.0,
           loadSettle / 1000.0);
    printf("Interval between keys: %d ms\n\n", interval);
    fflush(stdout);

    const QString sentence = QStringLiteral(
        "She lifted the lantern and the shadows ran from her like water, "
        "but the cold stayed where it was, patient and old.");

    if (parser.isSet("diagnose")) {
        diagnose(editor, document, text);
        return 0;
    }

    QList<Result> results;

    auto want = [&](const QString &name) {
        return only.isEmpty() || (only == name);
    };

    if (want("type-end-of-para")) {
        placeCursorInMiddle(editor, true);
        results.append(runScenario("type-end-of-para", editor, monitor, typeText(" " + sentence), interval));
        printResult(results.last());
    }

    if (want("type-mid-para")) {
        placeCursorInMiddle(editor, false);
        results.append(runScenario("type-mid-para", editor, monitor, typeText(sentence), interval));
        printResult(results.last());
    }

    if (want("enter-and-type")) {
        placeCursorInMiddle(editor, false);
        QList<KeyAction> actions;

        for (int i = 0; i < 12; ++i) {
            actions.append({KeyAction::Key, QChar(), Qt::Key_Return, Qt::NoModifier, {}});
            actions.append(typeText("Word here"));
        }

        results.append(runScenario("enter-and-type", editor, monitor, actions, interval));
        printResult(results.last());
    }

    if (want("backspace-repeat")) {
        placeCursorInMiddle(editor, false);
        QList<KeyAction> actions;

        for (int i = 0; i < 120; ++i) {
            actions.append({KeyAction::Key, QChar(), Qt::Key_Backspace, Qt::NoModifier, {}});
        }

        // Keyboard auto-repeat rate (~30 Hz).
        results.append(runScenario("backspace-repeat", editor, monitor, actions, 33));
        printResult(results.last());
    }

    if (want("join-lines")) {
        // Backspace at the start of blocks repeatedly joins lines.
        QList<KeyAction> actions;

        for (int i = 0; i < 16; ++i) {
            actions.append({KeyAction::Key, QChar(), Qt::Key_Down, Qt::NoModifier, {}});
            actions.append({KeyAction::Key, QChar(), Qt::Key_Home, Qt::NoModifier, {}});
            actions.append({KeyAction::Key, QChar(), Qt::Key_Backspace, Qt::NoModifier, {}});
        }

        placeCursorInMiddle(editor, false);
        results.append(runScenario("join-lines", editor, monitor, actions, interval));
        printResult(results.last());
    }

    if (want("paste-40-lines")) {
        QString chunk;

        for (int i = 0; i < 20; ++i) {
            chunk += QStringLiteral("Pasted line %1 with some *emphasis* and prose to spell check.\n\n").arg(i);
        }

        placeCursorInMiddle(editor, true);
        QList<KeyAction> actions;

        for (int i = 0; i < 6; ++i) {
            actions.append({KeyAction::Paste, QChar(), Qt::Key_V, Qt::ControlModifier, chunk});
        }

        results.append(runScenario("paste-40-lines", editor, monitor, actions, 400));
        printResult(results.last());
    }

    if (want("delete-selection")) {
        QList<KeyAction> actions;

        for (int i = 0; i < 6; ++i) {
            for (int j = 0; j < 8; ++j) {
                actions.append({KeyAction::Key, QChar(), Qt::Key_Down, Qt::ShiftModifier, {}});
            }

            actions.append({KeyAction::Key, QChar(), Qt::Key_Delete, Qt::NoModifier, {}});
        }

        placeCursorInMiddle(editor, false);
        results.append(runScenario("delete-selection", editor, monitor, actions, interval));
        printResult(results.last());
    }

    if (want("markdown-structure")) {
        // Typing that changes the structure of surrounding lines: open a
        // code fence (everything after becomes code), pause, close it again,
        // continue a list, underline a setext heading, and add emphasis that
        // spans two lines.
        placeCursorInMiddle(editor, true);
        QList<KeyAction> actions;
        auto enter = [&]() {
            actions.append({KeyAction::Key, QChar(), Qt::Key_Return, Qt::NoModifier, {}});
        };
        auto wait = [&](int keys) {
            for (int i = 0; i < keys; ++i) {
                actions.append({KeyAction::Key, QChar(), Qt::Key_Shift, Qt::NoModifier, {}});
            }
        };

        enter();
        enter();
        actions.append(typeText("```"));
        wait(12);
        enter();
        actions.append(typeText("code line"));
        enter();
        actions.append(typeText("```"));
        wait(12);
        enter();
        enter();
        actions.append(typeText("- alpha"));
        enter();
        actions.append(typeText("beta"));
        enter();
        enter();
        enter();
        actions.append(typeText("Title line"));
        enter();
        actions.append(typeText("==="));
        wait(12);
        enter();
        enter();
        actions.append(typeText("> quoted"));
        enter();
        enter();
        actions.append(typeText("start *of"));
        enter();
        actions.append(typeText("emphasis* end"));
        wait(12);

        results.append(runScenario("markdown-structure", editor, monitor, actions, interval));
        printResult(results.last());
    }

    settle(monitor);

    return (verifyHighlighting(editor, document) > 0) ? 2 : 0;
}
