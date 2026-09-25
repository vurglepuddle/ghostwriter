/*
 * SPDX-FileCopyrightText: 2014-2024 Megan Conkle <megan.conkle@kdemail.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <QApplication>
#include <QBrush>
#include <QColor>
#include <QDebug>
#include <QElapsedTimer>
#include <QFont>
#include <QObject>
#include <QPainter>
#include <QRegularExpression>
#include <QStack>
#include <QStaticText>
#include <QString>
#include <QStyle>
#include <QSyntaxHighlighter>
#include <QTextBlockFormat>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextLayout>
#include <QTimer>
#include <Qt>

#include "markdownhighlighter.h"
#include "markdownstates.h"
#include "textblockdata.h"

namespace ghostwriter
{
namespace
{
// Longest stretch of main-thread time spent re-highlighting after a parse
// before yielding back to the event loop, so typing is never held up.
constexpr qint64 RefreshSliceBudgetNs = 4 * 1000 * 1000;

bool isAscii(const QString &text)
{
    for (const QChar ch : text) {
        if (ch.unicode() >= 0x80) {
            return false;
        }
    }

    return true;
}

bool isBlank(QStringView text)
{
    for (const QChar ch : text) {
        if (!ch.isSpace()) {
            return false;
        }
    }

    return true;
}

// Number of UTF-8 bytes used to encode the character at index i, and the
// number of UTF-16 code units it occupies.
inline int utf8Length(const QString &text, int i, int &units)
{
    const char16_t ch = text.at(i).unicode();
    units = 1;

    if (ch < 0x80) {
        return 1;
    }

    if (ch < 0x800) {
        return 2;
    }

    if (QChar::isHighSurrogate(ch) && ((i + 1) < text.length()) && text.at(i + 1).isLowSurrogate()) {
        units = 2;
        return 4;
    }

    return 3;
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

// Maps an offset in a block's pre-edit text through an edit made at
// editPos that removed `removed` characters and added `added` characters.
// Starts inside the removed text move after the insertion; ends move before
// it, so text typed inside a formatted range inherits its formatting.
int mapStart(int pos, int editPos, int removed, int added)
{
    if (pos < editPos) {
        return pos;
    }

    if (pos >= (editPos + removed)) {
        return pos + added - removed;
    }

    return editPos + added;
}

int mapEnd(int pos, int editPos, int removed, int added)
{
    if (pos <= editPos) {
        return pos;
    }

    if (pos >= (editPos + removed)) {
        return pos + added - removed;
    }

    return editPos;
}
}

class MarkdownHighlighterPrivate
{
    Q_DISABLE_COPY(MarkdownHighlighterPrivate)
    Q_DECLARE_PUBLIC(MarkdownHighlighter)

public:
    MarkdownHighlighterPrivate(MarkdownHighlighter *highlighter) :
        q_ptr(highlighter),
        inBlockquote(false),
        useUnderlineForEmphasis(false)
    {
        ;
    }

    ~MarkdownHighlighterPrivate()
    {
        ;
    }

    /*
     * Result of highlighting one block from the Markdown AST, computed
     * without touching the document so that it can also be used to check
     * whether a block's current formatting is out of date.
     */
    struct BlockHighlight {
        TextBlockData::FormatOps ops;
        TextBlockData::MarkupRanges markup;
        int state = MarkdownStateUnknown;
        bool stateSet = false;
        QVector<int> rehighlightPositions;
        int structureType = -1;
        int structureOffset = 0;
        int structureLength = 0;
    };

    enum class BlockCheck {
        Outdated, // Formatting differs from the current AST.
        CurrentMoved, // Formatting is right, but the block's place in
                      // the document structure changed.
        Current // Nothing changed.
    };

    struct Context {
        QTextBlock block;
        const QString &text;
        bool ascii;
        int line;
        BlockHighlight &out;
    };

    struct LineRange {
        int first;
        int last;
    };

    MarkdownHighlighter *const q_ptr;

    ColorScheme colors;
    QTextCharFormat defaultFormat;
    MarkdownEditor *editor;
    MarkdownDocument *document;
    QRegularExpression heading1SetextRegex;
    QRegularExpression heading2SetextRegex;
    bool inBlockquote;
    QRegularExpression referenceDefinitionRegex;
    QRegularExpression inlineHtmlCommentRegex;
    QRegularExpression blockquoteRegex;
    QRegularExpression numberedListRegex;
    QRegularExpression bulletListRegex;
    QRegularExpression taskListRegex;
    bool useLargeHeadings;
    bool useUnderlineForEmphasis;
    bool italicizeBlockquotes;
    QColor spellingErrorColor;

    // Post-parse refresh work, processed in time slices.
    QTimer *refreshTimer;
    QVector<LineRange> refreshRanges;
    QTextBlock refreshTarget;

    bool isSetextHeadingState(const int state) const;
    bool lineMatchesNode(const int line, const MarkdownNode *const node) const;

    void computeHighlight(const QTextBlock &block, const QString &text, BlockHighlight &out) const;
    void applyFormattingForNode(Context &ctx, const MarkdownNode *const node) const;
    void setState(Context &ctx, int state) const;
    int provisionalState(const QString &text, int baseState) const;

    BlockCheck checkBlock(const QTextBlock &block);
    void refreshBlock(const QTextBlock &block);
    void processRefresh();
    void markAllStale();

    // NOTE: cmark-gfm returns column numbers and string lengths that are
    // actually byte number positions and number of bytes rather than UTF-8
    // character positions and character counts.  These methods will do the
    // necessary conversions between the MarkdownNode's position and length
    // and the current text block's character position and length.
    //
    int asciiToUtf8Pos(const Context &ctx, int pos) const;
    int asciiLenToUtf8Len(const Context &ctx,
                          int start, // Must be value returned by asciiToUtf8Pos
                          int length) const;
};

MarkdownHighlighter::MarkdownHighlighter
(
    MarkdownEditor *editor,
    const ColorScheme &colors
) : QSyntaxHighlighter(editor),
    d_ptr(new MarkdownHighlighterPrivate(this))
{
    Q_D(MarkdownHighlighter);

    d->colors = colors;
    d->editor = editor;
    d->document = static_cast<MarkdownDocument *>(editor->document());
    d->useUnderlineForEmphasis = false;
    d->italicizeBlockquotes = false;
    d->useLargeHeadings = false;
    d->inBlockquote = false;

    setDocument(editor->document());
    d->referenceDefinitionRegex.setPattern("^\\s*\\[(.+?)[^\\\\]\\]:");
    d->inlineHtmlCommentRegex.setPattern("^\\s*<\\!--.*-->\\s*$");
    d->blockquoteRegex.setPattern("^ {0,3}(>\\s*)+");
    d->numberedListRegex.setPattern("^\\s*([0-9]+)[.)]\\s+");
    d->bulletListRegex.setPattern("^\\s*[+*-]\\s+");
    d->taskListRegex.setPattern("^\\s*[-*+] \\[([x ])\\]\\s+");

    connect
    (
        this,
        SIGNAL(highlightBlockAtPosition(int)),
        this,
        SLOT(onHighlightBlockAtPosition(int)),
        Qt::QueuedConnection
    );

    d->refreshTimer = new QTimer(this);
    d->refreshTimer->setSingleShot(true);
    d->refreshTimer->setInterval(0);
    connect(d->refreshTimer, &QTimer::timeout, this, [d]() {
        d->processRefresh();
    });

    QFont font;
    font.setFamily("Monospace");
    font.setWeight(QFont::Normal);
    font.setItalic(false);
    font.setPointSizeF(12.0);
    font.setStyleStrategy(QFont::PreferAntialias);
    d->defaultFormat.setFont(font);
    d->defaultFormat.setForeground(QBrush(d->colors.foreground));
}

MarkdownHighlighter::~MarkdownHighlighter()
{
    ;
}

// Note:  Never set the QTextBlockFormat for a QTextBlock from within the
// highlighter.  Depending on how the block format is modified, a recursive call
// to the highlighter may be triggered, which will cause the application to
// crash.
//
// Likewise, don't try to set the QTextBlockFormat outside the highlighter
// (i.e., from within the text editor).  While the application will not crash,
// the format change will be added to the undo stack.  Attempting to undo from
// that point on will cause the undo stack to be virtually frozen, since undoing
// the format operation causes the text to be considered changed, thus
// triggering the slot that changes the text formatting to be triggered yet
// again.
//
void MarkdownHighlighter::highlightBlock(const QString &text)
{
    Q_D(MarkdownHighlighter);

    const QTextBlock block = currentBlock();
    const int oldState = block.userState();
    TextBlockData *data = blockData(block, true);

    // While the post-parse refresh re-highlights one block, QSyntaxHighlighter
    // would carry on through every following block whose state changes (all
    // the way to the end of the document after a code fence is opened).
    // Leave those blocks to the refresh, which works in time slices.
    const bool astCurrent = d->document->isMarkdownAstCurrent();
    const bool deferred = astCurrent && d->refreshTarget.isValid() && (block != d->refreshTarget);

    if (astCurrent && !deferred) {
        MarkdownHighlighterPrivate::BlockHighlight highlight;
        d->computeHighlight(block, text, highlight);

        for (const TextBlockData::FormatOp &op : std::as_const(highlight.ops)) {
            setFormat(op.start, op.length, op.format);
        }

        if (highlight.stateSet) {
            setCurrentBlockState(highlight.state);
        }

        data->formatOps = highlight.ops;
        data->markup = highlight.markup;
        data->highlightStale = false;
        data->structureType = highlight.structureType;
        data->structureOffset = highlight.structureOffset;
        data->structureLength = highlight.structureLength;

        for (int position : std::as_const(highlight.rehighlightPositions)) {
            emit highlightBlockAtPosition(position);
        }

        if (d->isSetextHeadingState(oldState) && !d->isSetextHeadingState(currentBlockState())) {
            QTextBlock previous = block;

            while (previous.previous().isValid() && (d->isSetextHeadingState(previous.previous().userState()))) {
                previous = previous.previous();
            }

            if (block != previous) {
                emit highlightBlockAtPosition(previous.position());
            }
        } else if (block.previous().isValid()
                   && (((MarkdownStatePipeTableDivider == (oldState & MarkdownStateMask))
                        && (MarkdownStatePipeTableDivider != (currentBlockState() & MarkdownStateMask)))
                       || ((MarkdownStatePipeTableDivider != (oldState & MarkdownStateMask))
                           && (MarkdownStatePipeTableDivider == (currentBlockState() & MarkdownStateMask))))) {
            emit highlightBlockAtPosition(block.previous().position());
        }
    } else {
        // The AST no longer matches the text (the user is typing and the
        // background parse has not caught up yet).  Re-apply the formatting
        // this block had, which adjustForEdit() already shifted to follow
        // the edit, instead of looking up nodes by line number in a stale
        // tree.  Keeping the block state of unedited blocks also stops
        // QSyntaxHighlighter from cascading through the rest of the document.
        for (const TextBlockData::FormatOp &op : std::as_const(data->formatOps)) {
            setFormat(op.start, op.length, op.format);
        }

        if (deferred) {
            // Keeping the state unchanged ends QSyntaxHighlighter's cascade.
            data->highlightStale = true;
        } else if (data->highlightStale) {
            const int baseState = (MarkdownStateUnknown != oldState) ? oldState : block.previous().userState();
            setCurrentBlockState(d->provisionalState(text, baseState));
        }
    }

    if (d->spellingErrorColor.isValid() && !data->misspellings.isEmpty()) {
        const int length = text.length();

        for (const TextBlockData::TextRange &range : std::as_const(data->misspellings)) {
            const int end = qMin(length, range.start + range.length);

            for (int i = qMax(0, range.start); i < end; i++) {
                QTextCharFormat spellingFormat = format(i);
                spellingFormat.setUnderlineStyle(QTextCharFormat::SpellCheckUnderline);
                spellingFormat.setUnderlineColor(d->spellingErrorColor);
                setFormat(i, 1, spellingFormat);
            }
        }
    }

    // The editor paints code block and blockquote backgrounds across whole
    // runs of blocks, so entering or leaving either affects its neighbors.
    if (0 != ((oldState ^ currentBlockState()) & (MarkdownStateCodeBlock | MarkdownStateBlockquote))) {
        d->editor->viewport()->update();
    }
}

void MarkdownHighlighter::adjustForEdit(int position, int charsRemoved, int charsAdded)
{
    Q_D(MarkdownHighlighter);

    QTextBlock first = d->document->findBlock(position);

    if (!first.isValid()) {
        return;
    }

    QTextBlock last = d->document->findBlock(position + charsAdded);

    if (!last.isValid()) {
        last = d->document->lastBlock();
    }

    // Qt keeps the user data of the block in which an edit starts, so its
    // cached formatting still describes the pre-edit text.  Shift it through
    // the edit, then hand each block produced by the edit its share.
    const int base = first.position();
    const int editPos = position - base;
    TextBlockData *firstData = blockData(first, true);

    TextBlockData::FormatOps shiftedOps;
    TextBlockData::MarkupRanges shiftedMarkup;
    QVector<TextBlockData::TextRange> shiftedMisspellings;

    // Length of the block's text before the edit, if it stayed one block.
    const int oldLength = (first == last) ? (first.length() - 1 - charsAdded + charsRemoved) : -1;

    for (const TextBlockData::FormatOp &op : std::as_const(firstData->formatOps)) {
        int start = mapStart(op.start, editPos, charsRemoved, charsAdded);
        const int end = mapEnd(op.start + op.length, editPos, charsRemoved, charsAdded);

        // Formatting of the whole line (e.g., a heading's font) also covers
        // text typed at the very start of the line.
        if ((0 == op.start) && (oldLength >= 0) && ((op.start + op.length) >= oldLength)) {
            start = 0;
        }

        if (end > start) {
            shiftedOps.append({start, end - start, op.format});
        }
    }

    for (const TextBlockData::MarkupRange &range : std::as_const(firstData->markup)) {
        const int start = mapStart(range.start, editPos, charsRemoved, charsAdded);
        const int end = mapEnd(range.end, editPos, charsRemoved, charsAdded);

        if (end > start) {
            shiftedMarkup.append({start, end, range.type});
        }
    }

    // A misspelling touched by the edit is dropped rather than shrunk; the
    // block is checked again once typing pauses.
    for (const TextBlockData::TextRange &range : std::as_const(firstData->misspellings)) {
        const int end = range.start + range.length;

        if (end < editPos) {
            shiftedMisspellings.append(range);
        } else if (range.start > (editPos + charsRemoved)) {
            shiftedMisspellings.append({range.start + charsAdded - charsRemoved, range.length});
        }
    }

    for (QTextBlock block = first; block.isValid(); block = block.next()) {
        TextBlockData *data = blockData(block, true);
        const int blockStart = block.position() - base;
        const int blockEnd = blockStart + block.length() - 1;

        data->formatOps.clear();
        data->markup.clear();
        data->misspellings.clear();

        for (const TextBlockData::FormatOp &op : std::as_const(shiftedOps)) {
            const int start = qMax(op.start, blockStart);
            const int end = qMin(op.start + op.length, blockEnd + 1);

            if (end > start) {
                data->formatOps.append({start - blockStart, end - start, op.format});
            }
        }

        for (const TextBlockData::MarkupRange &range : std::as_const(shiftedMarkup)) {
            if ((range.start >= blockStart) && (range.end <= (blockEnd + 1))) {
                data->markup.append({range.start - blockStart, range.end - blockStart, range.type});
            }
        }

        for (const TextBlockData::TextRange &range : std::as_const(shiftedMisspellings)) {
            if ((range.start >= blockStart) && ((range.start + range.length) <= blockEnd)) {
                data->misspellings.append({range.start - blockStart, range.length});
            }
        }

        data->highlightStale = true;

        if (block == last) {
            break;
        }
    }

    d->refreshTimer->stop();
}

void MarkdownHighlighter::refreshAfterParse()
{
    Q_D(MarkdownHighlighter);

    d->refreshRanges.clear();
    d->refreshTimer->stop();

    if (!d->document->isMarkdownAstCurrent()) {
        return;
    }

    MarkdownAST *ast = d->document->markdownAST();
    const int lastLine = d->document->blockCount() - 1;
    int number = 0;

    // Seed with every block whose text changed since it was last highlighted
    // from an up-to-date AST.  Inline and block context can change anywhere
    // within the enclosing top-level Markdown block (e.g., emphasis spanning
    // lines, a list becoming loose), so check all of its lines, plus one on
    // either side in case a neighbor was absorbed or split off.
    for (QTextBlock block = d->document->firstBlock(); block.isValid(); block = block.next(), number++) {
        TextBlockData *data = blockData(block, false);

        if ((nullptr != data) && !data->highlightStale) {
            continue;
        }

        int first = number;
        int last = number;
        MarkdownNode *top = (nullptr != ast) ? ast->topLevelBlockAtLine(number + 1) : nullptr;

        if ((nullptr != top) && (top->endLine() > 0)) {
            first = top->startLine() - 1;
            last = top->endLine() - 1;
        }

        // An edit can also change the Markdown block this line used to
        // belong to, e.g., a new thematic break ending the list above it.
        // Edited blocks still know where they sat before the edit.
        if ((nullptr != data) && (data->structureType >= 0)) {
            const int oldFirst = number - data->structureOffset;
            first = qMin(first, oldFirst);
            last = qMax(last, oldFirst + data->structureLength);
        }

        first = qBound(0, first - 1, lastLine);
        last = qBound(0, last + 1, lastLine);

        if (!d->refreshRanges.isEmpty() && (first <= (d->refreshRanges.last().last + 1))) {
            d->refreshRanges.last().first = qMin(d->refreshRanges.last().first, first);
            d->refreshRanges.last().last = qMax(d->refreshRanges.last().last, last);
        } else {
            d->refreshRanges.append({first, last});
        }
    }

    if (d->refreshRanges.isEmpty()) {
        return;
    }

    // Mark the whole of each range as needing a check.  The flag travels
    // with its block, so work interrupted by the next edit is picked up by
    // the next refresh no matter how the lines have shifted by then.
    for (const MarkdownHighlighterPrivate::LineRange &range : std::as_const(d->refreshRanges)) {
        QTextBlock block = d->document->findBlockByNumber(range.first);

        for (int line = range.first; block.isValid() && (line <= range.last); line++, block = block.next()) {
            blockData(block, true)->highlightStale = true;
        }
    }

    // Handle ranges on screen first.
    const int firstVisible = d->editor->cursorForPosition(QPoint(0, 0)).blockNumber();
    const int lastVisible = d->editor->cursorForPosition(QPoint(0, d->editor->viewport()->height())).blockNumber();

    std::stable_partition(d->refreshRanges.begin(), d->refreshRanges.end(), [firstVisible, lastVisible](const MarkdownHighlighterPrivate::LineRange &range) {
        return (range.last >= firstVisible) && (range.first <= lastVisible);
    });

    d->processRefresh();
}

void MarkdownHighlighter::increaseFontSize()
{
    Q_D(MarkdownHighlighter);

    d->defaultFormat.setFontPointSize(d->defaultFormat.fontPointSize() + 1.0);
    d->markAllStale();
    rehighlight();
}

void MarkdownHighlighter::decreaseFontSize()
{
    Q_D(MarkdownHighlighter);

    d->defaultFormat.setFontPointSize(d->defaultFormat.fontPointSize() - 1.0);
    d->markAllStale();
    rehighlight();
}

void MarkdownHighlighter::setColorScheme(const ColorScheme &colors)
{
    Q_D(MarkdownHighlighter);

    d->colors = colors;
    d->defaultFormat.setForeground(QBrush(colors.foreground));
    d->markAllStale();
    rehighlight();
}

void MarkdownHighlighter::setEnableLargeHeadingSizes(const bool enable)
{
    Q_D(MarkdownHighlighter);

    d->useLargeHeadings = enable;
    d->markAllStale();
    rehighlight();
}

void MarkdownHighlighter::setUseUnderlineForEmphasis(const bool enable)
{
    Q_D(MarkdownHighlighter);

    d->useUnderlineForEmphasis = enable;
    d->markAllStale();
    rehighlight();
}

void MarkdownHighlighter::setItalicizeBlockquotes(const bool enable)
{
    Q_D(MarkdownHighlighter);

    d->italicizeBlockquotes = enable;
    d->markAllStale();
    rehighlight();
}

void MarkdownHighlighter::setFont(const QString &fontFamily, const double fontSize)
{
    Q_D(MarkdownHighlighter);

    QFont font;
    font.setFamily(fontFamily);
    font.setWeight(QFont::Normal);
    font.setItalic(false);
    font.setPointSizeF(fontSize);
    d->defaultFormat.setFont(font);

    d->markAllStale();
    rehighlight();
}

QColor MarkdownHighlighter::spellingErrorColor() const
{
    Q_D(const MarkdownHighlighter);

    return d->spellingErrorColor;
}

void MarkdownHighlighter::setSpellingErrorColor(const QColor &color)
{
    Q_D(MarkdownHighlighter);

    if (d->spellingErrorColor == color) {
        return;
    }

    d->spellingErrorColor = color;

    for (QTextBlock block = document()->firstBlock(); block.isValid(); block = block.next()) {
        TextBlockData *data = blockData(block, false);

        if ((nullptr != data) && !data->misspellings.isEmpty()) {
            rehighlightBlock(block);
        }
    }
}

void MarkdownHighlighter::onHighlightBlockAtPosition(int position)
{
    QTextBlock block = document()->findBlock(position);
    rehighlightBlock(block);
}

void MarkdownHighlighterPrivate::markAllStale()
{
    for (QTextBlock block = document->firstBlock(); block.isValid(); block = block.next()) {
        TextBlockData *data = blockData(block, false);

        if (nullptr != data) {
            data->highlightStale = true;
        }
    }
}

MarkdownHighlighterPrivate::BlockCheck MarkdownHighlighterPrivate::checkBlock(const QTextBlock &block)
{
    TextBlockData *data = blockData(block, false);

    if (nullptr == data) {
        return BlockCheck::Outdated;
    }

    BlockHighlight highlight;
    computeHighlight(block, block.text(), highlight);

    if ((highlight.stateSet && (highlight.state != block.userState())) || (highlight.ops != data->formatOps)) {
        return BlockCheck::Outdated;
    }

    const bool moved = (highlight.structureType != data->structureType) || (highlight.structureOffset != data->structureOffset)
        || (highlight.structureLength != data->structureLength);

    // The block is displayed correctly; just record that it was verified.
    data->markup = highlight.markup;
    data->highlightStale = false;
    data->structureType = highlight.structureType;
    data->structureOffset = highlight.structureOffset;
    data->structureLength = highlight.structureLength;

    return moved ? BlockCheck::CurrentMoved : BlockCheck::Current;
}

void MarkdownHighlighterPrivate::refreshBlock(const QTextBlock &block)
{
    Q_Q(MarkdownHighlighter);

    refreshTarget = block;
    q->rehighlightBlock(block);
    refreshTarget = QTextBlock();
}

void MarkdownHighlighterPrivate::processRefresh()
{
    if (!document->isMarkdownAstCurrent()) {
        // Unprocessed blocks are still flagged as stale and will be seeded
        // again after the next parse.
        refreshRanges.clear();
        return;
    }

    QElapsedTimer budget;
    budget.start();

    auto outOfTime = [&budget]() {
        return budget.nsecsElapsed() > RefreshSliceBudgetNs;
    };

    while (!refreshRanges.isEmpty()) {
        LineRange &range = refreshRanges.first();
        QTextBlock block = document->findBlockByNumber(range.first);

        // Check every line in the range.
        while (block.isValid() && (range.first <= range.last)) {
            TextBlockData *data = blockData(block, false);

            if ((nullptr == data) || data->highlightStale) {
                if (BlockCheck::Outdated == checkBlock(block)) {
                    refreshBlock(block);
                }
            }

            range.first++;
            block = block.next();

            if (outOfTime() && (range.first <= range.last)) {
                refreshTimer->start();
                return;
            }
        }

        // Keep going past the range until the document structure is back in
        // step with what is displayed, e.g., after a code fence is opened or
        // closed, which changes the role of every fence that follows.
        while (block.isValid()) {
            const BlockCheck check = checkBlock(block);

            if (BlockCheck::Current == check) {
                break;
            }

            if (BlockCheck::Outdated == check) {
                refreshBlock(block);
            }

            range.first++;
            block = block.next();

            if (outOfTime()) {
                range.last = range.first;

                if (block.isValid()) {
                    blockData(block, true)->highlightStale = true;
                }

                refreshTimer->start();
                return;
            }
        }

        refreshRanges.removeFirst();
    }
}

void MarkdownHighlighterPrivate::computeHighlight(const QTextBlock &block, const QString &text, BlockHighlight &out) const
{
    Context ctx{block, text, isAscii(text), block.blockNumber() + 1, out};

    MarkdownAST *ast = document->markdownAST();
    MarkdownNode *node = nullptr;

    if (nullptr != ast) {
        node = ast->findBlockAtLine(ctx.line);

        const MarkdownNode *top = ast->topLevelBlockAtLine(ctx.line);

        if (nullptr != top) {
            out.structureType = top->type();
            out.structureOffset = ctx.line - top->startLine();
            out.structureLength = top->endLine() - top->startLine();
        }
    }

    if ((nullptr != node) && (MarkdownNode::Invalid != node->type())) {
        applyFormattingForNode(ctx, node);
    } else {
        if (isBlank(text)) {
            setState(ctx, MarkdownStateParagraphBreak);
        } else if (referenceDefinitionRegex.match(text).hasMatch()) {
            QTextCharFormat format = defaultFormat;
            format.setForeground(colors.link);

            out.ops.append({0, int(text.indexOf(':')), format});
            setState(ctx, MarkdownStateParagraph);
        } else if (inlineHtmlCommentRegex.match(text).hasMatch()) {
            QTextCharFormat format = defaultFormat;
            format.setForeground(colors.inlineHtml);
            out.ops.append({0, int(text.length()), format});

            const int previousState = block.previous().isValid() ? block.previous().userState() : MarkdownStateUnknown;

            if (previousState != MarkdownStateUnknown) {
                setState(ctx, previousState);
            } else {
                setState(ctx, MarkdownStateParagraph);
            }
        }
    }
}

void MarkdownHighlighterPrivate::setState(Context &ctx, int state) const
{
    ctx.out.state = state;
    ctx.out.stateSet = true;
}

int MarkdownHighlighterPrivate::provisionalState(const QString &text, int baseState) const
{
    // Code is code until the parser says otherwise.
    if ((MarkdownStateUnknown != baseState) && (MarkdownStateCodeBlock & baseState)) {
        return baseState;
    }

    int indent = 0;

    while ((indent < text.length()) && text.at(indent).isSpace()) {
        indent++;
    }

    int flags = 0;
    QStringView rest(text);
    QRegularExpressionMatch quote = blockquoteRegex.match(text);

    if (quote.hasMatch()) {
        flags |= MarkdownStateBlockquote;
        rest = rest.mid(quote.capturedLength());
    }

    const int baseMask = (MarkdownStateUnknown == baseState) ? MarkdownStateParagraphBreak : (baseState & MarkdownStateMask);
    int mask;

    if (taskListRegex.matchView(rest).hasMatch()) {
        mask = MarkdownStateTaskList;
    } else if (numberedListRegex.matchView(rest).hasMatch()) {
        mask = MarkdownStateNumberedList;
    } else if (bulletListRegex.matchView(rest).hasMatch()) {
        mask = MarkdownStateBulletPointList;
    } else if (isBlank(rest)) {
        mask = (0 != flags) ? MarkdownStateParagraph : MarkdownStateParagraphBreak;
    } else {
        switch (baseMask) {
        case MarkdownStateNumberedList:
        case MarkdownStateBulletPointList:
        case MarkdownStateTaskList:
            // Indented continuation of a list item.
            mask = (indent > 0) ? baseMask : MarkdownStateParagraph;
            break;
        case MarkdownStateParagraphBreak:
            mask = MarkdownStateParagraph;
            break;
        default:
            mask = baseMask;
            break;
        }
    }

    return mask | indent | flags;
}

void MarkdownHighlighterPrivate::applyFormattingForNode(Context &ctx, const MarkdownNode *const node) const
{
    MarkdownNode::NodeType type = node->type();

    int pos = 0;
    int length = 0;
    const int currentLine = ctx.line;
    const QString &text = ctx.text;
    const int blockLength = ctx.block.length();
    MarkdownState state = MarkdownStateParagraphBreak;

    QTextCharFormat baseFormat = defaultFormat;

    unsigned int indent = 0;

    for (int i = 0; i < text.length(); i++) {
        if (text[i].isSpace()) {
            indent++;
        } else {
            break;
        }
    }

    bool inBlockquote = node->isInsideBlockquote();

    if (inBlockquote) {
        baseFormat.setForeground(colors.blockquoteMarkup);
        baseFormat.setFontItalic(italicizeBlockquotes);

        ctx.out.ops.append({0, blockLength, baseFormat});

        baseFormat.setForeground(colors.blockquoteText);
    }

    // Do a pre-order traversal of the nodes.
    QStack<const MarkdownNode *> nodes;
    QStack<QTextCharFormat> nodeFormats;
    nodes.push(node);
    nodeFormats.push(baseFormat);

    while (!nodes.isEmpty()) {
        const MarkdownNode *current = nodes.pop();
        QTextCharFormat contextFormat;

        contextFormat = nodeFormats.pop();

        if (lineMatchesNode(currentLine, current)) {
            MarkdownNode::NodeType parentType = MarkdownNode::Invalid;

            if (nullptr != current->parent()) {
                parentType = current->parent()->type();
            }

            pos = asciiToUtf8Pos(ctx, current->position());
            length = asciiLenToUtf8Len(ctx, pos, current->length());
            type = current->type();

            if ((MarkdownNode::FootnoteDefinition == parentType)
                    || (MarkdownNode::FootnoteReference == parentType)) {
                type = parentType;
            }

            QTextCharFormat format = contextFormat;

            switch (type) {
            case MarkdownNode::Heading:
                length = blockLength;
                format.setFontWeight(QFont::Bold);
                contextFormat.setFontWeight(QFont::Bold);

                if (useLargeHeadings) {
                    format.setFontPointSize(format.fontPointSize()
                                            + (qreal)(7 - current->headingLevel()));
                    contextFormat.setFontPointSize(format.fontPointSize());
                }

                if (inBlockquote) {
                    format.setForeground(colors.blockquoteMarkup);
                    contextFormat.setForeground(colors.blockquoteText);
                } else {
                    format.setForeground(colors.headingMarkup);
                    contextFormat.setForeground(colors.headingText);
                }

                if (current->isSetextHeading()) {
                    switch (current->headingLevel()) {
                    case 1:
                        state = MarkdownStateSetextHeading1;
                        break;
                    case 2:
                        state = MarkdownStateSetextHeading2;
                        break;
                    default:
                        state = MarkdownStateUnknown;
                    }

                    // Rehighlight all blocks contained within this heading node.
                    if (currentLine != current->startLine()) {
                        QTextBlock block = document->findBlockByNumber(current->startLine() - 1);

                        if (block.isValid()) {
                            ctx.out.rehighlightPositions.append(block.position());
                        }
                    }
                } else {
                    switch (current->headingLevel()) {
                    case 1:
                        state = MarkdownStateAtxHeading1;
                        break;
                    case 2:
                        state = MarkdownStateAtxHeading2;
                        break;
                    case 3:
                        state = MarkdownStateAtxHeading3;
                        break;
                    case 4:
                        state = MarkdownStateAtxHeading4;
                        break;
                    case 5:
                        state = MarkdownStateAtxHeading5;
                        break;
                    case 6:
                        state = MarkdownStateAtxHeading6;
                        break;
                    default:
                        state = MarkdownStateUnknown;
                    }
                }

                break;
            case MarkdownNode::Text:
                break;
            case MarkdownNode::Paragraph:
                if (MarkdownStateUnknown == state) {
                    state = MarkdownStateParagraph;
                }

                break;
            case MarkdownNode::BlockQuote:
                format.setForeground(colors.blockquoteMarkup);
                format.setFontItalic(italicizeBlockquotes);
                contextFormat.setForeground(colors.blockquoteText);
                contextFormat.setFontItalic(italicizeBlockquotes);
                inBlockquote = true;
                break;
            case MarkdownNode::CodeBlock:
                if (current->isFencedCodeBlock() && ((currentLine == current->startLine()) || (currentLine == current->endLine()))) {
                    format.setForeground(colors.codeMarkup);
                    state = MarkdownStateCodeBlock;
                } else if ((currentLine == current->endLine()) && (current->length() <= 0)) {
                    state = MarkdownStateParagraphBreak;
                } else {
                    format.setForeground(colors.codeText);
                    length = blockLength - pos + 1;
                    state = MarkdownStateCodeBlock;
                }

                break;
            case MarkdownNode::ListItem:
                format.setForeground(colors.listMarkup);
                format.setFontWeight(QFont::Bold);

                if (current->isNumberedListItem()) {
                    state = MarkdownStateNumberedList;
                } else { // Assume bullet list item
                    state = MarkdownStateBulletPointList;
                }

                break;
            case MarkdownNode::TaskListItem:
                state = MarkdownStateTaskList;
                format.setForeground(colors.listMarkup);
                format.setFontWeight(QFont::Bold);
                break;
            case MarkdownNode::Emph:
                format.setForeground(colors.emphasisMarkup);

                if (useUnderlineForEmphasis) {
                    contextFormat.setFontUnderline(true);
                } else {
                    contextFormat.setFontItalic(true);
                    format.setFontItalic(true);
                }

                contextFormat.setForeground(colors.emphasisText);
                ctx.out.markup.append({pos, pos + length, MarkdownNode::Emph});
                break;
            case MarkdownNode::Strong:
                contextFormat.setForeground(colors.emphasisText);
                contextFormat.setFontWeight(QFont::Bold);
                format.setForeground(colors.emphasisMarkup);
                format.setFontWeight(QFont::Bold);
                ctx.out.markup.append({pos, pos + length, MarkdownNode::Strong});
                break;
            case MarkdownNode::Code: {
                int backticks = 0;

                for (int i = pos - 1; i >= 0; i--) {
                    if (text[i] == QChar('`')) {
                        backticks++;
                    } else {
                        break;
                    }
                }

                format.setForeground(colors.codeMarkup);
                ctx.out.ops.append({pos - backticks, length + (2 * backticks), format});
                format.setForeground(colors.codeText);
                break;
            }
            case MarkdownNode::HtmlInline:
                format.setForeground(colors.inlineHtml);
                contextFormat.setForeground(colors.inlineHtml);
                break;
            case MarkdownNode::Link:
                format.setForeground(colors.link);
                contextFormat.setForeground(colors.link);
                break;
            case MarkdownNode::Image:
                format.setForeground(colors.image);
                contextFormat.setForeground(colors.image);
                break;
            case MarkdownNode::ThematicBreak:
                format.setForeground(colors.divider);
                state = MarkdownStateHorizontalRule;
                break;
            case MarkdownNode::FootnoteReference:
                format.setForeground(colors.link);
                contextFormat.setForeground(colors.link);
                break;
            case MarkdownNode::FootnoteDefinition:
                format.setForeground(colors.link);
                contextFormat.setForeground(colors.link);
                state = MarkdownStateParagraph;
                break;
            case MarkdownNode::TableHeading:
                format.setForeground(colors.emphasisMarkup);
                pos = 0;
                length = blockLength;
                contextFormat.setFontWeight(QFont::Bold);
                state = MarkdownStatePipeTableHeader;
                break;
            case MarkdownNode::TableRow:
                format.setForeground(colors.emphasisMarkup);
                pos = 0;
                length = blockLength;
                state = MarkdownStatePipeTableRow;
                break;
            case MarkdownNode::TableCell:
                format = contextFormat;

                if ((nullptr != current->parent())
                        && (MarkdownNode::TableHeading
                            == current->parent()->type())) {
                    format.setFontWeight(QFont::Bold);
                }
                break;
            case MarkdownNode::Table:
                format.setForeground(colors.emphasisMarkup);
                pos = 0;
                length = blockLength;
                state = MarkdownStatePipeTableDivider;
                break;
            case MarkdownNode::Strikethrough:
                format.setForeground(colors.emphasisMarkup);
                contextFormat.setFontStrikeOut(true);
                ctx.out.markup.append({pos, pos + length, MarkdownNode::Strikethrough});
                break;
            default:
                if (referenceDefinitionRegex.match(text).hasMatch()) {
                    pos = 0;
                    length = text.indexOf(':') + 1;
                    format.setForeground(colors.link);
                } else if (inBlockquote) {
                    format.setForeground(colors.blockquoteMarkup);
                }

                break;
            }

            if ((length <= 0) || (length > blockLength)) {
                length = blockLength;
            }

            ctx.out.ops.append({pos, length, format});

            if (MarkdownNode::TaskListItem == type) {
                format = contextFormat;
                format.setForeground(colors.link);

                int checkboxStart = text.indexOf('[');
                int checkboxEnd = text.indexOf(']');

                ctx.out.ops.append({checkboxStart, checkboxEnd - checkboxStart + 1, format});
            }
        }

        MarkdownNode *child = current->lastChild();

        while ((nullptr != child) && (!child->isInvalid())) {
            nodes.push(child);
            nodeFormats.push(contextFormat);
            child = child->previous();
        }
    }

    if (MarkdownStateUnknown != state) {
        state |= indent;

        if (inBlockquote) {
            state |= MarkdownStateBlockquote;
        }

        setState(ctx, state);
    }
}

int MarkdownHighlighterPrivate::asciiToUtf8Pos(const Context &ctx, int pos) const
{
    const QString &text = ctx.text;

    if (pos < 0) {
        return 0;
    }

    if (ctx.ascii) {
        return qMin(pos, int(text.length()));
    }

    int current = 0;
    int i = 0;

    while (i < text.length()) {
        if (current == pos) {
            return i;
        }

        int units;
        current += utf8Length(text, i, units);
        i += units;
    }

    return current;
}

int MarkdownHighlighterPrivate::asciiLenToUtf8Len(const Context &ctx, int start, int length) const
{
    const QString &text = ctx.text;
    int count = 0;

    if (text.length() <= 0) {
        return 0;
    }

    if ((start < 0) || (start >= text.length())) {
        return 0;
    }

    if (length < 0) {
        return 0;
    }

    if (ctx.ascii) {
        if (0 == length) {
            return 1;
        }

        if ((start + length) <= text.length()) {
            return length;
        }

        return text.length() - start + 1;
    }

    int i = start;

    while (i < text.length()) {
        int units;
        count += utf8Length(text, i, units);
        i += units;

        if (count >= length) {
            return i - start;
        }
    }

    return text.length() - start + 1;
}

bool MarkdownHighlighterPrivate::lineMatchesNode(const int line, const MarkdownNode *const node) const
{
    return ((node->isBlockType()
                && (line >= node->startLine())
                && ((line <= node->endLine()) || (0 == node->endLine())))
            || (node->isInlineType()
                && ((line == node->startLine())
                    || (line == node->endLine())
                    || (0 == node->endLine()))));
}

bool MarkdownHighlighterPrivate::isSetextHeadingState(const int state) const
{
    switch (state & MarkdownStateMask) {
    case MarkdownStateSetextHeading1:
    case MarkdownStateSetextHeading2:
        return true;
    default:
        return false;
    }
}
} // namespace ghostwriter
