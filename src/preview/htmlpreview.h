/*
 * SPDX-FileCopyrightText: 2014-2023 Megan Conkle <megan.conkle@kdemail.net>
 * SPDX-FileCopyrightText: 2026 ghostwriter contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef HTML_PREVIEW_H
#define HTML_PREVIEW_H

#include <QScopedPointer>
#include <QString>
#include <QWidget>

#include "editor/markdowndocument.h"
#include "export/exporter.h"

namespace ghostwriter
{
/**
 * Live HTML Preview window.
 */
class HtmlPreviewPrivate;
class HtmlPreview : public QWidget
{
    Q_OBJECT
    Q_DECLARE_PRIVATE(HtmlPreview)

public:
    /**
     * Constructor.  Takes text document to be rendered as HTML as
     * parameter.
     */
    HtmlPreview(MarkdownDocument *document, Exporter *exporter, QWidget *parent = nullptr);

    /**
     * Destructor.
     */
    virtual ~HtmlPreview();

public slots:
    /**
     * Call this method to re-render the HTML for the document.
     */
    void updatePreview();

    /**
     * Call this method to navigate to the HTML heading tag (h1 - h6)
     * having the given sequence number.  For example, to navigate to the
     * very first heading in the document, pass in a value of 1.  To go
     * to the second heading that appears in the document, pass in a value
     * of 2, etc.
     */
    void navigateToHeading(int headingSequenceNumber);

    /**
     * Call this method to set the HTML exporter used in
     * generating HTML from the Markdown document.
     */
    void setHtmlExporter(Exporter *exporter);

    /**
     * Call this method to change the CSS style sheet code.
     */
    void setStyleSheet(const QString &css);

    /**
     * Call this method to enable or disable math rendering.
     */
    void setMathEnabled(bool enabled);

    /**
     * Allows remote images and media for the currently open document.
     */
    void allowRemoteContent();

    /**
     * Blocks remote content and clears the current document's permission.
     */
    void resetRemoteContentPermission();

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    QScopedPointer<HtmlPreviewPrivate> d_ptr;
};
} // namespace ghostwriter

#endif
