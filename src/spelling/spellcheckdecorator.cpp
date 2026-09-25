/*
 * SPDX-FileCopyrightText: 2022-2023 Megan Conkle <megan.conkle@kdemail.net>
 * SPDX-FileCopyrightText: 2006 Jacob R Rideout <kde@jacobrideout.net>
   SPDX-FileCopyrightText: 2006 Martin Sandsmark <martin.sandsmark@kde.org>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <QAction>
#include <QContextMenuEvent>
#include <QDeadlineTimer>
#include <QElapsedTimer>
#include <QHash>
#include <QList>
#include <QLocale>
#include <QMenu>
#include <QSet>
#include <QStringList>
#include <QTextBlock>
#include <QTextBoundaryFinder>
#include <QTextCursor>
#include <QTimer>
#include <QVector>

#include <Sonnet/BackgroundChecker>
#include <Sonnet/Dialog>
#include <Sonnet/GuessLanguage>
#include <Sonnet/Settings>
#include <Sonnet/Speller>

#include "../editor/markdowneditor.h"
#include "../editor/markdownhighlighter.h"
#include "../editor/textblockdata.h"
#include "spellcheckdecorator.h"
#include "spellcheckdialog.h"

namespace ghostwriter
{
namespace
{
// How long typing must pause before spelling is checked again.  Checking is
// never done while keys are being pressed.
constexpr int IdleDelayMs = 600;

// Delay after loading or pasting a large amount of text.
constexpr int LargeChangeDelayMs = 100;

// Longest stretch of main-thread time spent checking before yielding back
// to the event loop.
constexpr qint64 SliceBudgetNs = 5 * 1000 * 1000;

constexpr int MaxCachedWordsPerLanguage = 200000;

QString normalizedLanguage(const QString &language)
{
    QString normalized = language;
    normalized.replace('_', '-');
    return normalized;
}

// Dictionary names such as "en-029" (English, Caribbean) are not always
// understood by QLocale, in which case fall back to the language alone.
QLocale localeFor(const QString &dictionary)
{
    QLocale locale(dictionary);

    if (QLocale::C == locale.language()) {
        locale = QLocale(normalizedLanguage(dictionary).section('-', 0, 0));
    }

    return locale;
}

// Maps the scripts that matter for choosing a dictionary to QLocale's
// script enumeration.
QLocale::Script localeScript(QChar::Script script)
{
    switch (script) {
    case QChar::Script_Latin:
        return QLocale::LatinScript;
    case QChar::Script_Cyrillic:
        return QLocale::CyrillicScript;
    case QChar::Script_Greek:
        return QLocale::GreekScript;
    case QChar::Script_Arabic:
        return QLocale::ArabicScript;
    case QChar::Script_Hebrew:
        return QLocale::HebrewScript;
    case QChar::Script_Armenian:
        return QLocale::ArmenianScript;
    case QChar::Script_Georgian:
        return QLocale::GeorgianScript;
    case QChar::Script_Devanagari:
        return QLocale::DevanagariScript;
    case QChar::Script_Bengali:
        return QLocale::BengaliScript;
    case QChar::Script_Thai:
        return QLocale::ThaiScript;
    case QChar::Script_Hangul:
        return QLocale::HangulScript;
    default:
        return QLocale::AnyScript;
    }
}

TextBlockData *blockData(const QTextBlock &block, bool create)
{
    TextBlockData *data = static_cast<TextBlockData *>(block.userData());

    if ((nullptr == data) && create) {
        data = new TextBlockData();
        const_cast<QTextBlock &>(block).setUserData(data);
    }

    return data;
}
}

class SpellCheckDecoratorPrivate
{
    Q_DECLARE_PUBLIC(SpellCheckDecorator)

public:
    struct Position {
        int start, length;
    };

    /**
     * This structure abstracts the positions of breaks in the test. As per the
     * unicode annex, both the start and end of the text are returned.
     */
    typedef QList<Position> Positions;

    SpellCheckDecoratorPrivate(SpellCheckDecorator *decorator)
    : q_ptr(decorator)
    {
        ;
    }

    ~SpellCheckDecoratorPrivate()
    {
        qDeleteAll(spellers);
    }

    static Sonnet::Settings *settings;

    SpellCheckDecorator *q_ptr;
    QPlainTextEdit *editor;
    MarkdownHighlighter *highlighter;
    QColor errorColor;
    SpellCheckDialog *spellCheckDialog;

    // Background checking.
    QTimer *idleTimer;
    QTimer *sliceTimer;
    QTextBlock passStart;
    QTextBlock nextBlock;
    bool passWrapped;
    quint64 generation;

    // When a check became due before the spell checker was loaded.
    bool checkPending;
    QDeadlineTimer pendingCheckDeadline;

    // Dictionaries and results, which are reused across blocks and passes.
    QHash<QString, Sonnet::Speller *> spellers;
    QHash<QString, QHash<QString, bool>> wordCache;
    struct ScriptLanguages {
        QStringList dictionaries;
        QSet<QLocale::Language> languages;
        QString preferred;
    };

    bool languageMapBuilt;
    QString defaultLanguage;
    QLocale::Language defaultPrimaryLanguage;
    QHash<int, ScriptLanguages> languagesByScript;

    void loadSettings();
    bool enabled() const;
    void invalidateAll();
    void scheduleCheck(int delayMs);
    void startPass();
    void processSlice();
    void clearAllMisspellings();
    void checkBlock(const QTextBlock &block);
    QVector<TextBlockData::TextRange> findMisspellings(const QString &text, const QString &language);
    bool isMisspelled(const QString &word, const QString &language);
    Sonnet::Speller *spellerFor(const QString &language);
    void buildLanguageMap();
    QString languageFor(const QString &text, TextBlockData *data, size_t textHash);

    QMenu * createContextMenu();

    QMenu *createSpellingMenu(const QString &misspelledWord, const QTextCursor &cursorForWord);

    QString getMisspelledWordAtCursor(QTextCursor &cursorForWord) const;

    void onContentsChanged(int position, int charsRemoved, int charsAdded);
    Positions wordBreaks(const QString &text) const;
};

Sonnet::Settings *SpellCheckDecoratorPrivate::settings = nullptr;

SpellCheckDecorator::SpellCheckDecorator(QPlainTextEdit *editor)
: QObject(editor),
  d_ptr(new SpellCheckDecoratorPrivate(this))
{
    Q_D(SpellCheckDecorator);

    // Note: Sonnet's settings are loaded later (see loadSpellChecker()).

    d->editor = editor;
    d->highlighter = nullptr;
    d->spellCheckDialog = nullptr;
    d->passWrapped = false;
    d->generation = 1;
    d->checkPending = false;
    d->languageMapBuilt = false;
    d->defaultPrimaryLanguage = QLocale::AnyLanguage;

    Q_ASSERT(nullptr != d->editor);

    if (MarkdownEditor *markdownEditor = qobject_cast<MarkdownEditor *>(d->editor)) {
        d->highlighter = qobject_cast<MarkdownHighlighter *>(markdownEditor->highlighter());
    }

    d->editor->installEventFilter(this);
    d->editor->viewport()->installEventFilter(this);

    connect(d->editor->document(),
        static_cast<void (QTextDocument::*)(int, int, int)>(
            &QTextDocument::contentsChange),
        this,
        [d](int position, int charsRemoved, int charsAdded) {
            d->onContentsChanged(position, charsRemoved, charsAdded);
        }
    );

    d->idleTimer = new QTimer(this);
    d->idleTimer->setSingleShot(true);
    connect(d->idleTimer, &QTimer::timeout, this, [d]() {
        d->startPass();
    });

    d->sliceTimer = new QTimer(this);
    d->sliceTimer->setSingleShot(true);
    d->sliceTimer->setInterval(0);
    connect(d->sliceTimer, &QTimer::timeout, this, [d]() {
        d->processSlice();
    });
}

SpellCheckDecorator::~SpellCheckDecorator()
{
    ;
}

QColor SpellCheckDecorator::errorColor() const
{
    Q_D(const SpellCheckDecorator);

    return d->errorColor;
}

void SpellCheckDecorator::loadSpellChecker()
{
    Q_D(SpellCheckDecorator);

    d->loadSettings();
}

void SpellCheckDecorator::setErrorColor(const QColor &color)
{
    Q_D(SpellCheckDecorator);

    d->errorColor = color;

    if (nullptr != d->highlighter) {
        d->highlighter->setSpellingErrorColor(color);
    }
}

void SpellCheckDecorator::settingsChanged()
{
    Q_D(SpellCheckDecorator);

    if (d->settings) {
        delete d->settings;
        d->settings = new Sonnet::Settings(this);
    }

    qDeleteAll(d->spellers);
    d->spellers.clear();
    d->languageMapBuilt = false;
    this->rehighlight();
}

void SpellCheckDecorator::rehighlight() const
{
    SpellCheckDecoratorPrivate *d = const_cast<SpellCheckDecoratorPrivate *>(d_func());

    d->loadSettings();
    d->invalidateAll();

    if (d->enabled()) {
        d->scheduleCheck(0);
    } else {
        d->clearAllMisspellings();
    }
}

bool SpellCheckDecorator::eventFilter(QObject *watched, QEvent *event)
{
    Q_D(SpellCheckDecorator);
    Q_UNUSED(watched);

    if (event->type() != QEvent::ContextMenu) {
        return false;
    }

    d->loadSettings();

    if (!d->settings->checkerEnabledByDefault() || d->editor->isReadOnly()) {
        return false;
    }

    // Check spelling of text block under mouse or at cursor position.
    QContextMenuEvent *contextEvent = static_cast<QContextMenuEvent *>(event);

    QTextCursor cursorForWord;

    // If the context menu event was triggered by pressing the menu key, use the
    // current text cursor rather than the event position to get a cursor
    // position, since the event position is the mouse position rather than the
    // text cursor position.
    if (QContextMenuEvent::Keyboard == contextEvent->reason()) {
        cursorForWord = d->editor->textCursor();
    }
    // Else process as mouse event.
    else {
        cursorForWord = d->editor->cursorForPosition(contextEvent->pos());
    }

    QMenu *popupMenu = d->createContextMenu();
    QString misspelledWord = d->getMisspelledWordAtCursor(cursorForWord);

    // If the selected word is spelled correctly, use the default processing for
    // the context menu.
    if (!misspelledWord.isNull() && !misspelledWord.isEmpty()) {
        popupMenu->addMenu(
            d->createSpellingMenu(misspelledWord,
            cursorForWord));
    }

    // Show context menu
    QPoint menuPos;

    // If event was triggered by a key press, use the text cursor coordinates to
    // display the popup menu.
    if (QContextMenuEvent::Keyboard == contextEvent->reason()) {
        QRect cr = d->editor->cursorRect();
        menuPos.setX(cr.x());
        menuPos.setY(cr.y() + (cr.height() / 2));
        menuPos = d->editor->viewport()->mapToGlobal(menuPos);
    }
    // Else use the mouse coordinates from the context menu event.
    else {
        menuPos = d->editor->viewport()->mapToGlobal(contextEvent->pos());
    }

    popupMenu->exec(menuPos);

    if (nullptr != popupMenu) {
        delete popupMenu;
        popupMenu = nullptr;
    }

    return true;
}

void SpellCheckDecoratorPrivate::loadSettings()
{
    if (nullptr == settings) {
        settings = new Sonnet::Settings();
    }

    if (checkPending) {
        checkPending = false;

        if (enabled()) {
            scheduleCheck(int(pendingCheckDeadline.remainingTime()));
        }
    }
}

bool SpellCheckDecoratorPrivate::enabled() const
{
    return (nullptr != highlighter) && settings->checkerEnabledByDefault();
}

void SpellCheckDecoratorPrivate::invalidateAll()
{
    // Blocks compare their stored generation against this one, so bumping it
    // makes every block due for a recheck without touching them now.
    generation++;
    wordCache.clear();
}

void SpellCheckDecoratorPrivate::scheduleCheck(int delayMs)
{
    sliceTimer->stop();
    idleTimer->start(delayMs);
}

void SpellCheckDecoratorPrivate::onContentsChanged(int position, int charsRemoved, int charsAdded)
{
    Q_UNUSED(position)

    const int delayMs = ((charsRemoved + charsAdded) > 4096) ? LargeChangeDelayMs : IdleDelayMs;

    // Until the spell checker is loaded, only remember when the check is due.
    if (nullptr == settings) {
        checkPending = true;
        pendingCheckDeadline.setRemainingTime(delayMs);
        return;
    }

    if (!enabled()) {
        return;
    }

    // The highlighter has already shifted this block's known misspellings to
    // follow the edit.  Checking waits until typing pauses.
    scheduleCheck(delayMs);
}

void SpellCheckDecoratorPrivate::startPass()
{
    if (!enabled()) {
        clearAllMisspellings();
        return;
    }

    // Start on screen, run to the end of the document, then wrap around.
    passStart = editor->cursorForPosition(QPoint(0, 0)).block();

    if (!passStart.isValid()) {
        passStart = editor->document()->firstBlock();
    }

    nextBlock = passStart;
    passWrapped = false;
    processSlice();
}

void SpellCheckDecoratorPrivate::processSlice()
{
    if (!enabled()) {
        return;
    }

    QElapsedTimer budget;
    budget.start();

    while (budget.nsecsElapsed() < SliceBudgetNs) {
        if (!nextBlock.isValid()) {
            if (passWrapped) {
                return;
            }

            passWrapped = true;
            nextBlock = editor->document()->firstBlock();
        }

        if (passWrapped && (nextBlock == passStart)) {
            return;
        }

        checkBlock(nextBlock);
        nextBlock = nextBlock.next();
    }

    sliceTimer->start();
}

void SpellCheckDecoratorPrivate::clearAllMisspellings()
{
    sliceTimer->stop();
    idleTimer->stop();

    for (QTextBlock block = editor->document()->firstBlock(); block.isValid(); block = block.next()) {
        TextBlockData *data = blockData(block, false);

        if (nullptr == data) {
            continue;
        }

        data->spellCheckValid = false;

        if (!data->misspellings.isEmpty()) {
            data->misspellings.clear();

            if (nullptr != highlighter) {
                highlighter->rehighlightBlock(block);
            }
        }
    }
}

void SpellCheckDecoratorPrivate::checkBlock(const QTextBlock &block)
{
    TextBlockData *data = blockData(block, true);
    const QString text = block.text();
    const size_t textHash = qHash(text);

    if (data->spellCheckValid && (data->spellCheckTextHash == textHash) && (data->spellCheckGeneration == generation)) {
        return;
    }

    const QVector<TextBlockData::TextRange> misspellings = findMisspellings(text, languageFor(text, data, textHash));

    data->spellCheckValid = true;
    data->spellCheckTextHash = textHash;
    data->spellCheckGeneration = generation;

    // Only blocks whose underlines actually change need to be re-highlighted
    // (and thus laid out again).
    if (misspellings != data->misspellings) {
        data->misspellings = misspellings;
        highlighter->rehighlightBlock(block);
    }
}

QVector<TextBlockData::TextRange> SpellCheckDecoratorPrivate::findMisspellings(const QString &text, const QString &language)
{
    QVector<TextBlockData::TextRange> misspellings;

    for (const Position &word : wordBreaks(text)) {
        if (isMisspelled(text.mid(word.start, word.length), language)) {
            misspellings.append({word.start, word.length});
        }
    }

    return misspellings;
}

bool SpellCheckDecoratorPrivate::isMisspelled(const QString &word, const QString &language)
{
    // Prose repeats the same words constantly, and each dictionary lookup
    // can be a comparatively slow call into the platform spell checker.
    QHash<QString, bool> &cache = wordCache[language];
    auto it = cache.constFind(word);

    if (it != cache.constEnd()) {
        return it.value();
    }

    if (cache.size() > MaxCachedWordsPerLanguage) {
        cache.clear();
    }

    const bool misspelled = spellerFor(language)->isMisspelled(word);
    cache.insert(word, misspelled);
    return misspelled;
}

Sonnet::Speller *SpellCheckDecoratorPrivate::spellerFor(const QString &language)
{
    Sonnet::Speller *speller = spellers.value(language);

    if (nullptr == speller) {
        speller = new Sonnet::Speller(language);
        spellers.insert(language, speller);
    }

    return speller;
}

void SpellCheckDecoratorPrivate::buildLanguageMap()
{
    if (languageMapBuilt) {
        return;
    }

    languageMapBuilt = true;
    languagesByScript.clear();
    defaultLanguage = settings->defaultLanguage();
    defaultPrimaryLanguage = localeFor(defaultLanguage).language();

    const QStringList available = spellerFor(defaultLanguage)->availableLanguages();

    for (const QString &dictionary : available) {
        const QLocale locale = localeFor(dictionary);

        if (QLocale::C == locale.language()) {
            continue;
        }

        ScriptLanguages &entry = languagesByScript[int(locale.script())];
        entry.dictionaries.append(dictionary);
        entry.languages.insert(locale.language());
    }

    for (ScriptLanguages &entry : languagesByScript) {
        entry.preferred = entry.dictionaries.first();

        if (entry.languages.size() == 1) {
            // A language's main variant, e.g., ru-RU rather than ru-MO.
            const QString main = normalizedLanguage(QLocale(*entry.languages.constBegin()).name());

            for (const QString &dictionary : std::as_const(entry.dictionaries)) {
                if (normalizedLanguage(dictionary) == main) {
                    entry.preferred = dictionary;
                    break;
                }
            }
        }
    }
}

QString SpellCheckDecoratorPrivate::languageFor(const QString &text, TextBlockData *data, size_t textHash)
{
    buildLanguageMap();

    if (!settings->autodetectLanguage()) {
        return defaultLanguage;
    }

    if (!data->language.isEmpty() && (data->languageTextHash == textHash)) {
        return data->language;
    }

    // Detecting the language of prose is only necessary when the text is
    // written in a script for which dictionaries in more than one language
    // are installed.  Otherwise the script alone decides, which is far
    // cheaper than Sonnet's guesser: it checks words against every installed
    // dictionary, and so would against each regional variant of the same
    // language (en-US, en-GB, en-AU, ...) just to break a tie.
    QHash<int, int> scriptCounts;
    int bestCount = 0;
    QChar::Script bestScript = QChar::Script_Unknown;

    for (const QChar ch : text) {
        if (!ch.isLetter()) {
            continue;
        }

        const QChar::Script script = ch.script();
        const int count = ++scriptCounts[script];

        if (count > bestCount) {
            bestCount = count;
            bestScript = script;
        }
    }

    QString language = defaultLanguage;
    const QLocale::Script script = localeScript(bestScript);
    auto entry = languagesByScript.constFind(int(script));

    if ((0 == bestCount) || (QLocale::AnyScript == script) || (entry == languagesByScript.constEnd())) {
        language = defaultLanguage;
    } else if (entry->languages.size() == 1) {
        language = entry->languages.contains(defaultPrimaryLanguage) ? defaultLanguage : entry->preferred;
    } else {
        const QString guess = Sonnet::GuessLanguage().identify(text, entry->dictionaries);

        if (!guess.isEmpty()) {
            language = guess;
        }
    }

    data->language = language;
    data->languageTextHash = textHash;
    return language;
}

QMenu * SpellCheckDecoratorPrivate::createContextMenu()
{
    Q_Q(SpellCheckDecorator);

    // Add spell check action to the standard context menu that comes with
    // the editor.
    MarkdownEditor *markdownEditor = qobject_cast<MarkdownEditor *>(this->editor);
    QMenu *popupMenu = markdownEditor
        ? markdownEditor->createStandardContextMenu()
        : this->editor->createStandardContextMenu();

    QAction *checkSpellingAction =
        new QAction(SpellCheckDecorator::tr("Check spelling..."), popupMenu);
    checkSpellingAction->setEnabled(!markdownEditor
        || !markdownEditor->blindDraftModeEnabled());

    q->connect(checkSpellingAction,
        &QAction::triggered,
        q,
        [this, q]() {
            spellCheckDialog = new SpellCheckDialog(this->editor);
            q->connect(
                spellCheckDialog,
                &SpellCheckDialog::finished,
                q,
                &SpellCheckDecorator::rehighlight
            );
            spellCheckDialog->show();
        }
    );

    popupMenu->addAction(checkSpellingAction);

    return popupMenu;
}

QMenu *SpellCheckDecoratorPrivate::createSpellingMenu(const QString &misspelledWord, const QTextCursor &cursorForWord)
{
    Q_Q(SpellCheckDecorator);
    QMenu *spellingMenu = new QMenu(SpellCheckDecorator::tr("Spelling"));

    buildLanguageMap();

    TextBlockData *data = blockData(cursorForWord.block(), false);
    const QString language = ((nullptr != data) && !data->language.isEmpty()) ? data->language : defaultLanguage;
    Sonnet::Speller *speller = spellerFor(language);

    QStringList suggestions = speller->suggest(misspelledWord);

    QAction *addWordToDictionaryAction =
        new QAction(SpellCheckDecorator::tr("Add word to dictionary"),
            spellingMenu);

    q->connect(addWordToDictionaryAction, &QAction::triggered, [this, q, speller, cursorForWord, misspelledWord]() {
        this->editor->setTextCursor(cursorForWord);
        speller->addToPersonal(misspelledWord);
        q->rehighlight();
    });

    spellingMenu->addAction(addWordToDictionaryAction);
    spellingMenu->addSeparator();

    if (!suggestions.empty()) {
        // Add suggested spellings to the popup menu.
        for (const QString &suggestion : suggestions) {
            QAction *suggestionAction = new QAction(suggestion, spellingMenu);

            // If a suggested spelling is selected from the popup menu,
            // replace the misspelled word selection with the new spelling.
            q->connect(suggestionAction,
                &QAction::triggered,
                [cursorForWord, suggestionAction]() {
                    QTextCursor cursor(cursorForWord);
                    cursor.insertText(suggestionAction->data().toString());
                }
            );

            // Need the following line because KDE Plasma 5 will insert a hidden
            // ampersand into the menu text as a keyboard accelerator.  Go off
            // of the data in the QAction rather than the text to avoid this.
            suggestionAction->setData(suggestion);

            // Add suggested spelling action to the popup menu.
            spellingMenu->addAction(suggestionAction);
        }
    } else {
        QAction *noSuggestionsAction = new QAction(
            SpellCheckDecorator::tr("No spelling suggestions found"),
            spellingMenu);
        noSuggestionsAction->setEnabled(false);
        spellingMenu->addAction(noSuggestionsAction);
    }

    return spellingMenu;
}


QString SpellCheckDecoratorPrivate::getMisspelledWordAtCursor(
    QTextCursor &cursorForWord) const
{
    TextBlockData *data = blockData(cursorForWord.block(), false);

    if (nullptr == data) {
        return QString();
    }

    const int blockPosition = cursorForWord.positionInBlock();

    for (const TextBlockData::TextRange &range : std::as_const(data->misspellings)) {
        if ((blockPosition >= range.start) && (blockPosition <= (range.start + range.length))) {
            // Select the misspelled word.
            cursorForWord.setPosition(cursorForWord.block().position() + range.start);
            cursorForWord.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor, range.length);

            return cursorForWord.selectedText();
        }
    }

    return QString();
}

// Code is lifted from KDE Frameworks' Sonnet library, because we know it
// just works.  :)
SpellCheckDecoratorPrivate::Positions
SpellCheckDecoratorPrivate::wordBreaks(const QString &text) const
{
    Positions breaks;

    if (text.isEmpty()) {
        return breaks;
    }

    QTextBoundaryFinder boundaryFinder(QTextBoundaryFinder::Word, text);

    while (boundaryFinder.position() < text.length()) {
        if (!(boundaryFinder.boundaryReasons().testFlag(
                QTextBoundaryFinder::StartOfItem))) {
            if (boundaryFinder.toNextBoundary() == -1) {
                break;
            }
            continue;
        }

        Position pos;
        pos.start = boundaryFinder.position();
        int end = boundaryFinder.toNextBoundary();
        if (end == -1) {
            break;
        }
        pos.length = end - pos.start;
        if (pos.length < 1) {
            continue;
        }
        breaks.append(pos);

        if (boundaryFinder.toNextBoundary() == -1) {
            break;
        }
    }
    return breaks;
}

} // namespace ghostwriter
