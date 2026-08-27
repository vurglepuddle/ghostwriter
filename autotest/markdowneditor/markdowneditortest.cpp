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
#include <QTest>
#include <QTextBlock>
#include <QTextCursor>

#include "editor/colorscheme.h"
#include "editor/markdowndocument.h"
#include "editor/markdowneditor.h"

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

QTEST_MAIN(MarkdownEditorTest)

#include "markdowneditortest.moc"
