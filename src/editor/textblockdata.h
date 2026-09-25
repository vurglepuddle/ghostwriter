/*
 * SPDX-FileCopyrightText: 2014-2023 Megan Conkle <megan.conkle@kdemail.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef TEXTBLOCKDATA_H
#define TEXTBLOCKDATA_H

#include <QObject>
#include <QTextBlock>
#include <QTextBlockUserData>
#include <QTextCharFormat>
#include <QVector>

#include "markdown/markdownast.h"

#include "markdowndocument.h"

namespace ghostwriter
{
/**
 * User data for use with the MarkdownHighlighter, SpellCheckDecorator and
 * DocumentStatistics.  Each of these caches its per-block results here so
 * that work is only redone for blocks whose text actually changed.
 */
class TextBlockData : public QTextBlockUserData
{
public:

    typedef struct MarkupRange
    {
        int start;
        int end;
        MarkdownNode::NodeType type;
    } MarkupRange;

    typedef QVector<MarkupRange> MarkupRanges;

    /**
     * A single QSyntaxHighlighter::setFormat() call, recorded so that the
     * block's Markdown formatting can be re-applied (or shifted to follow an
     * edit) without consulting a possibly stale Markdown AST.
     */
    typedef struct FormatOp {
        int start;
        int length;
        QTextCharFormat format;

        bool operator==(const FormatOp &other) const
        {
            return (start == other.start) && (length == other.length) && (format == other.format);
        }
    } FormatOp;

    typedef QVector<FormatOp> FormatOps;

    typedef struct TextRange {
        int start;
        int length;

        bool operator==(const TextRange &other) const
        {
            return (start == other.start) && (length == other.length);
        }
    } TextRange;

    /**
     * Constructor.
     */
    TextBlockData()
    {
        wordCount = 0;
        alphaNumericCharacterCount = 0;
        sentenceCount = 0;
        lixLongWordCount = 0;
        statisticsValid = false;
        statisticsTextHash = 0;
        highlightStale = true;
        spellCheckValid = false;
        spellCheckTextHash = 0;
        spellCheckGeneration = 0;
        languageTextHash = 0;
    }

    /**
     * Destructor.
     */
    virtual ~TextBlockData()
    {
        ;
    }

    void clearMarkup()
    {
        markup.clear();
    }

    // Statistics cache.
    int wordCount;
    int alphaNumericCharacterCount;
    int sentenceCount;
    int lixLongWordCount;
    bool statisticsValid;
    size_t statisticsTextHash;

    // Markdown highlighting.  highlightStale is set whenever the block's text
    // changes and cleared once the block is highlighted from an AST that
    // matches the document text.
    MarkupRanges markup;
    FormatOps formatOps;
    bool highlightStale;

    // Where the block sat in the document structure when it was last
    // highlighted: the type of the enclosing top-level Markdown block, the
    // block's line offset within it, and that block's line count.  Used to
    // tell when re-highlighting after an edit can stop, since a line (e.g.,
    // a code fence) can look the same while its role has changed.
    int structureType = -2;
    int structureOffset = 0;
    int structureLength = 0;

    // Spell checking.
    QVector<TextRange> misspellings;
    bool spellCheckValid;
    size_t spellCheckTextHash;
    quint64 spellCheckGeneration;
    QString language;
    size_t languageTextHash;
};
} // namespace ghostwriter

#endif // TEXTBLOCKDATA_H
