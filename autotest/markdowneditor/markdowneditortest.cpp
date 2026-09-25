/*
 * SPDX-FileCopyrightText: 2026 ghostwriter contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <QApplication>
#include <QAction>
#include <QClipboard>
#include <QMenu>
#include <QPlainTextEdit>
#include <QScopedPointer>
#include <QSignalSpy>
#include <QTest>
#include <QTextBlock>
#include <QTextCursor>

#include <QRandomGenerator>
#include <QSyntaxHighlighter>
#include <QTextLayout>

#include "editor/colorscheme.h"
#include "editor/markdowndocument.h"
#include "editor/markdowneditor.h"
#include "markdown/cmarkgfmapi.h"

using namespace ghostwriter;

namespace
{
ColorScheme testColors()
{
    ColorScheme colors;
    colors.foreground = Qt::black;
    colors.background = Qt::white;
    colors.selection = Qt::blue;
    colors.cursor = Qt::black;
    colors.link = Qt::blue;
    colors.image = Qt::darkBlue;
    colors.inlineHtml = Qt::darkGreen;
    colors.headingText = Qt::black;
    colors.headingMarkup = Qt::gray;
    colors.emphasisText = Qt::black;
    colors.emphasisMarkup = Qt::gray;
    colors.blockquoteText = Qt::darkGray;
    colors.blockquoteMarkup = Qt::gray;
    colors.divider = Qt::gray;
    colors.listMarkup = Qt::gray;
    colors.codeText = Qt::black;
    colors.codeMarkup = Qt::gray;
    colors.error = Qt::red;
    return colors;
}

void moveToBlock(MarkdownEditor &editor, int blockNumber, bool atEnd = true)
{
    QTextCursor cursor(editor.document()->findBlockByNumber(blockNumber));

    if (atEnd) {
        cursor.movePosition(QTextCursor::EndOfBlock);
    }

    editor.navigateDocumentForLoad(cursor.position());
}

// Format applied by the highlighter at a position in the block.  Later
// ranges take precedence, as when the layout draws them.
QTextCharFormat formatAt(const QTextBlock &block, int position)
{
    QTextCharFormat format;

    for (const QTextLayout::FormatRange &range : block.layout()->formats()) {
        if ((position >= range.start) && (position < (range.start + range.length))) {
            format.merge(range.format);
        }
    }

    return format;
}

// Returns a description of the first block whose incrementally maintained
// highlighting differs from highlighting everything from a fresh parse, or
// an empty string if there is none.
QString compareWithFullRehighlight(MarkdownEditor &editor, MarkdownDocument &document)
{
    QList<QList<QTextLayout::FormatRange>> formats;
    QList<int> states;

    for (QTextBlock block = document.firstBlock(); block.isValid(); block = block.next()) {
        formats.append(block.layout()->formats());
        states.append(block.userState());
    }

    document.setMarkdownAST(CmarkGfmAPI::instance()->parse(document.toPlainText(), false));
    editor.highlighter()->rehighlight();

    int number = 0;

    for (QTextBlock block = document.firstBlock(); block.isValid(); block = block.next(), number++) {
        if ((block.userState() != states.at(number)) || (block.layout()->formats() != formats.at(number))) {
            auto describe = [](const QList<QTextLayout::FormatRange> &ranges) {
                QStringList parts;

                for (const QTextLayout::FormatRange &range : ranges) {
                    parts.append(QString("[%1+%2 %3 w%4]")
                                     .arg(range.start)
                                     .arg(range.length)
                                     .arg(range.format.foreground().color().name())
                                     .arg(range.format.fontWeight()));
                }

                return parts.join(' ');
            };

            return QString(
                       "block %1 (\"%2\", previous \"%3\"): state %4, expected %5; "
                       "formats %6, expected %7")
                .arg(number)
                .arg(block.text(), block.previous().text())
                .arg(states.at(number), 0, 16)
                .arg(block.userState(), 0, 16)
                .arg(describe(formats.at(number)), describe(block.layout()->formats()));
        }
    }

    return QString();
}

int visibleBlockCount(const MarkdownEditor &editor)
{
    int count = 0;

    for (QTextBlock block = editor.document()->firstBlock();
         block.isValid(); block = block.next()) {
        count += block.isVisible() ? 1 : 0;
    }

    return count;
}
}

class MarkdownEditorTest : public QObject
{
    Q_OBJECT

private slots:
    void enablesInExistingDocumentAndRestoresIt();
    void preventsNavigationAndCrossLineSelection();
    void commitsLinesAndProtectsBoundaries();
    void handlesEmptyAndConsecutiveBlankLines();
    void treatsWrappedRowsAsOneDocumentLine();
    void constrainsCopyAndCommitsMultilinePaste();
    void limitsUndoRedoToCurrentDraftLine();
    void resetsCurrentLineWhenLoadingDocument();
    void combinesWithFocusAndHemingwayModes();
    void updatesAstAfterDebouncedEdits();
    void keepsFormattingInPlaceWhileAstIsStale();
    void continuesListBeforeParseCatchesUp();
    void incrementalHighlightingMatchesFullRehighlight();
    void incrementalHighlightingSurvivesRandomEdits_data();
    void incrementalHighlightingSurvivesRandomEdits();
};

void MarkdownEditorTest::enablesInExistingDocumentAndRestoresIt()
{
    MarkdownDocument document;
    MarkdownEditor editor(&document, testColors());
    editor.setPlainText("first\nsecond\nthird");
    moveToBlock(editor, 1);

    const QString originalText = document.toPlainText();
    document.setModified(false);
    editor.setBlindDraftModeEnabled(true);

    QVERIFY(editor.blindDraftModeEnabled());
    QCOMPARE(editor.textCursor().blockNumber(), 1);
    QCOMPARE(visibleBlockCount(editor), 1);
    QVERIFY(!document.findBlockByNumber(0).isVisible());
    QVERIFY(document.findBlockByNumber(1).isVisible());
    QVERIFY(!document.findBlockByNumber(2).isVisible());
    QCOMPARE(document.toPlainText(), originalText);
    QVERIFY(!document.isModified());

    editor.setBlindDraftModeEnabled(false);

    QVERIFY(!editor.blindDraftModeEnabled());
    QCOMPARE(visibleBlockCount(editor), 3);
    editor.navigateDocument(0);
    QCOMPARE(editor.textCursor().position(), 0);
    QCOMPARE(document.toPlainText(), originalText);
    QVERIFY(!document.isModified());
}

void MarkdownEditorTest::preventsNavigationAndCrossLineSelection()
{
    MarkdownDocument document;
    MarkdownEditor editor(&document, testColors());
    editor.setPlainText("first\nsecond\nthird");
    moveToBlock(editor, 1);
    editor.setBlindDraftModeEnabled(true);

    editor.navigateDocument(0);
    QCOMPARE(editor.textCursor().blockNumber(), 1);

    QTextCursor hiddenCursor(document.findBlockByNumber(0));
    hiddenCursor.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
    editor.QPlainTextEdit::setTextCursor(hiddenCursor);
    QCOMPARE(editor.textCursor().blockNumber(), 1);
    QVERIFY(!editor.textCursor().hasSelection());

    editor.selectAll();
    QVERIFY(editor.textCursor().selectionStart() >= editor.blindDraftLineStart());
    QVERIFY(editor.textCursor().selectionEnd() <= editor.blindDraftLineEnd());

    editor.show();
    editor.setFocus();
    QTest::keyClick(&editor, Qt::Key_Home, Qt::ControlModifier);
    QCOMPARE(editor.textCursor().blockNumber(), 1);
    QTest::keyClick(&editor, Qt::Key_Up);
    QCOMPARE(editor.textCursor().blockNumber(), 1);
    QTest::keyClick(&editor, Qt::Key_PageUp);
    QCOMPARE(editor.textCursor().blockNumber(), 1);

    QTest::mouseClick(editor.viewport(), Qt::LeftButton, Qt::NoModifier, QPoint(1, 1));
    QCOMPARE(editor.textCursor().blockNumber(), 1);

    QTest::keyClick(&editor, Qt::Key_Up, Qt::ShiftModifier);
    QVERIFY(editor.textCursor().selectionStart() >= editor.blindDraftLineStart());
    QVERIFY(editor.textCursor().selectionEnd() <= editor.blindDraftLineEnd());
}

void MarkdownEditorTest::commitsLinesAndProtectsBoundaries()
{
    MarkdownDocument document;
    MarkdownEditor editor(&document, testColors());
    editor.setPlainText("before\ncurrent\nafter");
    moveToBlock(editor, 1);
    editor.setBlindDraftModeEnabled(true);
    editor.show();
    editor.setFocus();

    QTest::keyClick(&editor, Qt::Key_Return);
    QCOMPARE(editor.textCursor().blockNumber(), 2);
    QCOMPARE(visibleBlockCount(editor), 1);
    QVERIFY(!document.findBlockByNumber(1).isVisible());
    QVERIFY(document.findBlockByNumber(2).isVisible());

    QTest::keyClicks(&editor, "ab");
    QTest::keyClick(&editor, Qt::Key_Backspace);
    QCOMPARE(editor.textCursor().block().text(), QString("a"));
    QTest::keyClick(&editor, Qt::Key_Home);
    QTest::keyClick(&editor, Qt::Key_Delete);
    QCOMPARE(editor.textCursor().block().text(), QString());

    const QString committedText = document.toPlainText();
    QTest::keyClick(&editor, Qt::Key_Backspace);
    QCOMPARE(document.toPlainText(), committedText);

    QTextCursor endCursor = editor.textCursor();
    endCursor.movePosition(QTextCursor::EndOfBlock);
    editor.setTextCursor(endCursor);
    QTest::keyClick(&editor, Qt::Key_Delete);
    QCOMPARE(document.toPlainText(), committedText);
}

void MarkdownEditorTest::handlesEmptyAndConsecutiveBlankLines()
{
    MarkdownDocument document;
    MarkdownEditor editor(&document, testColors());
    editor.setPlainText("");
    editor.setBlindDraftModeEnabled(true);
    editor.show();
    editor.setFocus();

    QTest::keyClick(&editor, Qt::Key_Return);
    QTest::keyClick(&editor, Qt::Key_Enter);

    QCOMPARE(document.toPlainText(), QString("\n\n"));
    QCOMPARE(document.blockCount(), 3);
    QCOMPARE(editor.textCursor().blockNumber(), 2);
    QCOMPARE(visibleBlockCount(editor), 1);

    QTest::keyClick(&editor, Qt::Key_Backspace);
    QCOMPARE(document.toPlainText(), QString("\n\n"));
}

void MarkdownEditorTest::treatsWrappedRowsAsOneDocumentLine()
{
    MarkdownDocument document;
    MarkdownEditor editor(&document, testColors());
    editor.resize(80, 240);
    editor.setLineWrapMode(QPlainTextEdit::WidgetWidth);
    editor.setPlainText(
        "This is a very long document line that wraps across many visual rows "
        "without containing a newline.\nhidden");
    moveToBlock(editor, 0);
    editor.setBlindDraftModeEnabled(true);
    editor.show();
    editor.setFocus();

    QTest::keyClick(&editor, Qt::Key_Up);
    QCOMPARE(editor.textCursor().blockNumber(), 0);
    QTest::keyClick(&editor, Qt::Key_Down);
    QCOMPARE(editor.textCursor().blockNumber(), 0);
    QCOMPARE(visibleBlockCount(editor), 1);
    QVERIFY(document.firstBlock().isVisible());
}

void MarkdownEditorTest::constrainsCopyAndCommitsMultilinePaste()
{
    MarkdownDocument document;
    MarkdownEditor editor(&document, testColors());
    editor.setPlainText("before\n");
    moveToBlock(editor, 1);
    editor.setBlindDraftModeEnabled(true);
    editor.show();
    editor.setFocus();

    QApplication::clipboard()->setText("one\n\nthree");
    editor.paste();

    QCOMPARE(document.toPlainText(), QString("before\none\n\nthree"));
    QCOMPARE(editor.textCursor().blockNumber(), 3);
    QCOMPARE(visibleBlockCount(editor), 1);

    const QString afterPaste = document.toPlainText();
    editor.undo();
    QCOMPARE(document.toPlainText(), afterPaste);

    editor.selectAll();
    editor.copy();
    QCOMPARE(QApplication::clipboard()->text(), QString("three"));
}

void MarkdownEditorTest::limitsUndoRedoToCurrentDraftLine()
{
    MarkdownDocument document;
    MarkdownEditor editor(&document, testColors());
    editor.setPlainText("committed\ncurrent");
    moveToBlock(editor, 1);
    editor.setBlindDraftModeEnabled(true);
    editor.show();
    editor.setFocus();

    QTest::keyClicks(&editor, "x");
    QCOMPARE(document.toPlainText(), QString("committed\ncurrentx"));
    editor.undo();
    QCOMPARE(document.toPlainText(), QString("committed\ncurrent"));
    editor.redo();
    QCOMPARE(document.toPlainText(), QString("committed\ncurrentx"));

    QTest::keyClick(&editor, Qt::Key_Return);
    const QString afterCommit = document.toPlainText();
    editor.undo();
    QCOMPARE(document.toPlainText(), afterCommit);

    QScopedPointer<QMenu> contextMenu(editor.createStandardContextMenu());
    QAction *undoAction = contextMenu->findChild<QAction *>("edit-undo");
    QVERIFY(undoAction);
    QVERIFY(!undoAction->isEnabled());
}

void MarkdownEditorTest::resetsCurrentLineWhenLoadingDocument()
{
    MarkdownDocument document;
    MarkdownEditor editor(&document, testColors());
    editor.setPlainText("old\ndocument");
    moveToBlock(editor, 1);
    editor.setBlindDraftModeEnabled(true);

    editor.setPlainText("new first\nnew second\nnew third");
    QTextBlock thirdBlock = document.findBlockByNumber(2);
    editor.navigateDocumentForLoad(thirdBlock.position() + 3);

    QCOMPARE(editor.textCursor().blockNumber(), 2);
    QCOMPARE(visibleBlockCount(editor), 1);
    QVERIFY(thirdBlock.isVisible());

    editor.navigateDocument(0);
    QCOMPARE(editor.textCursor().blockNumber(), 2);
    QCOMPARE(document.toPlainText(), QString("new first\nnew second\nnew third"));
}

void MarkdownEditorTest::combinesWithFocusAndHemingwayModes()
{
    MarkdownDocument document;
    MarkdownEditor editor(&document, testColors());
    editor.setPlainText("first\nsecond");
    moveToBlock(editor, 1);
    editor.setFocusMode(FocusModeCurrentLine);
    editor.setHemingWayModeEnabled(true);
    editor.setBlindDraftModeEnabled(true);
    editor.show();
    editor.setFocus();

    QCOMPARE(editor.focusMode(), FocusModeCurrentLine);
    QVERIFY(editor.hemingwayModeEnabled());
    QCOMPARE(visibleBlockCount(editor), 1);

    const QString originalText = document.toPlainText();
    QTest::keyClick(&editor, Qt::Key_Backspace);
    QCOMPARE(document.toPlainText(), originalText);

    QTest::keyClick(&editor, Qt::Key_Return);
    QCOMPARE(editor.textCursor().blockNumber(), 2);
    QCOMPARE(visibleBlockCount(editor), 1);

    editor.setBlindDraftModeEnabled(false);
    QCOMPARE(editor.focusMode(), FocusModeCurrentLine);
    QVERIFY(editor.hemingwayModeEnabled());
    QCOMPARE(visibleBlockCount(editor), 3);

    const QString afterBlindDraft = document.toPlainText();
    QTest::keyClick(&editor, Qt::Key_Backspace);
    QCOMPARE(document.toPlainText(), afterBlindDraft);
}

void MarkdownEditorTest::updatesAstAfterDebouncedEdits()
{
    MarkdownDocument document;
    MarkdownEditor editor(&document, testColors());
    editor.setPlainText("# Before\nbody");
    QSignalSpy astChangedSpy(&editor, &MarkdownEditor::markdownAstChanged);

    QTextCursor cursor(&document);
    cursor.movePosition(QTextCursor::End);
    cursor.insertText("\n## After");

    QTRY_VERIFY_WITH_TIMEOUT(astChangedSpy.count() > 0, 2000);
    QVERIFY(document.markdownAST());
    QCOMPARE(document.markdownAST()->headings().size(), 2);
}

void MarkdownEditorTest::keepsFormattingInPlaceWhileAstIsStale()
{
    MarkdownDocument document;
    MarkdownEditor editor(&document, testColors());
    editor.setPlainText("Some **bold** word");

    const QTextBlock block = document.firstBlock();
    const int boldPosition = block.text().indexOf("bold");
    QCOMPARE(formatAt(block, boldPosition).fontWeight(), int(QFont::Bold));
    QVERIFY(formatAt(block, 0).fontWeight() != int(QFont::Bold));

    // Typing before the bold text must move its formatting along with it,
    // even though the parser has not caught up yet.
    QTextCursor cursor(&document);
    cursor.insertText("Hello ");

    QVERIFY(!document.isMarkdownAstCurrent());
    QCOMPARE(formatAt(block, boldPosition + 6).fontWeight(), int(QFont::Bold));
    QVERIFY(formatAt(block, boldPosition).fontWeight() != int(QFont::Bold));

    QTRY_VERIFY_WITH_TIMEOUT(document.isMarkdownAstCurrent(), 2000);
    QCOMPARE(formatAt(block, boldPosition + 6).fontWeight(), int(QFont::Bold));
}

void MarkdownEditorTest::continuesListBeforeParseCatchesUp()
{
    MarkdownDocument document;
    MarkdownEditor editor(&document, testColors());
    editor.setPlainText("");
    editor.show();
    editor.setFocus();

    // Press Enter straight after typing, before the background parse runs.
    QTest::keyClicks(&editor, "- item");
    QTest::keyClick(&editor, Qt::Key_Return);

    QVERIFY(!document.isMarkdownAstCurrent());
    QCOMPARE(document.toPlainText(), QString("- item\n- "));
}

void MarkdownEditorTest::incrementalHighlightingMatchesFullRehighlight()
{
    MarkdownDocument document;
    MarkdownEditor editor(&document, testColors());
    editor.resize(600, 400);
    editor.show();

    QString text;

    for (int i = 0; i < 6; i++) {
        text += QStringLiteral(
                    "Heading %1\n=========\n\n"
                    "Some *emphasis that\nspans lines* and **strong** and `code`.\n\n"
                    "- item\n  - nested *item*\n- [ ] task\n\n"
                    "> quoted **text**\n> more\n\n"
                    "```\nint x = 0;\n```\n\n"
                    "| a | b |\n|---|---|\n| 1 | 2 |\n\n")
                    .arg(i);
    }

    editor.setPlainText(text);

    auto blockAt = [&](int number) {
        return document.findBlockByNumber(number);
    };

    auto waitForParse = [&]() {
        QTRY_VERIFY_WITH_TIMEOUT(document.isMarkdownAstCurrent(), 2000);
        QTest::qWait(100);
    };

    // Typing in a paragraph and splitting it.
    QTextCursor cursor(blockAt(4));
    cursor.movePosition(QTextCursor::EndOfBlock);
    cursor.insertText(" more words");
    cursor.insertText("\nnew line");
    waitForParse();

    // Joining lines several times without waiting in between.
    for (int i = 0; i < 3; i++) {
        QTextCursor join(blockAt(10 + i));
        join.deletePreviousChar();
    }

    waitForParse();

    // Opening a code fence turns everything after it into code...
    QTextCursor fence(blockAt(20));
    fence.insertText("```\n");
    waitForParse();

    // ...and closing it again turns it back.
    QTextCursor closing(blockAt(26));
    closing.insertText("```\n");
    waitForParse();

    // A multi-line paste and a deleted selection spanning several blocks.
    QTextCursor paste(blockAt(40));
    paste.insertText("Pasted *line*\n\n- one\n- two\n\nSetext\n---\n");

    QTextCursor selection(blockAt(55));
    selection.setPosition(blockAt(60).position() + 3, QTextCursor::KeepAnchor);
    selection.removeSelectedText();

    // An underline making the previous line a setext heading.
    QTextCursor setext(blockAt(70));
    setext.insertText("Now a heading\n===\n");
    waitForParse();
    QTest::qWait(300);

    const QString mismatch = compareWithFullRehighlight(editor, document);
    QVERIFY2(mismatch.isEmpty(), qPrintable(mismatch));
}

void MarkdownEditorTest::incrementalHighlightingSurvivesRandomEdits_data()
{
    QTest::addColumn<quint32>("seed");

    for (quint32 seed : {20260925u, 9u, 1234567u}) {
        QTest::newRow(qPrintable(QString::number(seed))) << seed;
    }
}

void MarkdownEditorTest::incrementalHighlightingSurvivesRandomEdits()
{
    QFETCH(quint32, seed);

    MarkdownDocument document;
    MarkdownEditor editor(&document, testColors());
    editor.resize(600, 400);
    editor.show();

    QString text;

    for (int i = 0; i < 4; i++) {
        text += QStringLiteral(
            "Heading\n=======\n\nSome *emphasis that\nspans lines* and **strong**.\n\n"
            "- item\n  - nested\n- [ ] task\n\n> quote\n> more\n\n"
            "```\ncode\n```\n\n| a | b |\n|---|---|\n| 1 | 2 |\n\nplain text\n\n");
    }

    editor.setPlainText(text);

    const QStringList fragments = {"```\n", "```", "- ",    "* ",    "1. ",         "> ",   "**",    "*",         "_",  "`",      "\n",
                                   "\n\n",  "# ",  "===\n", "---\n", "| x | y |\n", "    ", "word ", "[link](x)", "~~", "- [ ] ", "<!-- c -->"};

    QRandomGenerator rng(seed);

    for (int step = 0; step < 60; step++) {
        const int length = document.characterCount() - 1;
        QTextCursor cursor(&document);
        cursor.setPosition(rng.bounded(length + 1));

        QString edit;

        if ((rng.bounded(4) == 0) && (length > 10)) {
            const int end = qMin(length, cursor.position() + 1 + int(rng.bounded(30)));
            edit = QString("delete %1 to %2").arg(cursor.position()).arg(end);
            cursor.setPosition(end, QTextCursor::KeepAnchor);
            cursor.removeSelectedText();
        } else {
            QString fragment = fragments.at(rng.bounded(fragments.size()));
            edit = QString("insert at %1").arg(cursor.position());
            cursor.insertText(fragment);
        }

        // Sometimes let the parser catch up, sometimes keep editing.
        if (rng.bounded(3) == 0) {
            QTRY_VERIFY_WITH_TIMEOUT(document.isMarkdownAstCurrent(), 2000);
        }

        if ((step % 15) == 14) {
            QTRY_VERIFY_WITH_TIMEOUT(document.isMarkdownAstCurrent(), 2000);
            QTest::qWait(150);

            const QString mismatch = compareWithFullRehighlight(editor, document);
            QVERIFY2(mismatch.isEmpty(), qPrintable(QString("after step %1 (%2): %3").arg(step).arg(edit, mismatch)));
        }
    }
}

QTEST_MAIN(MarkdownEditorTest)

#include "markdowneditortest.moc"
