/*
 * SPDX-FileCopyrightText: 2014-2024 Megan Conkle <megan.conkle@kdemail.net>
 * SPDX-FileCopyrightText: 2026 ghostwriter contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <QAction>
#include <QApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFuture>
#include <QFutureWatcher>
#include <QMenu>
#include <QString>
#include <QTextStream>
#include <QVBoxLayout>
#include <QVariant>
#include <QWebChannel>
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
#include <QWebEnginePermission>
#endif
#include <QWebEngineProfile>
#include <QWebEngineSettings>
#include <QWebEngineView>
#include <QtConcurrentRun>

#include <KMessageWidget>

#include "htmlpreview.h"
#include "previewproxy.h"
#include "remotecontentinterceptor.h"
#include "sandboxedwebpage.h"
#include <export/exporter.h>

namespace ghostwriter
{
class HtmlPreviewPrivate
{
    Q_DECLARE_PUBLIC(HtmlPreview)

public:
    HtmlPreviewPrivate(HtmlPreview *q_ptr)
        : q_ptr(q_ptr)
    {
        proxy = new PreviewProxy(q_ptr);
    }

    ~HtmlPreviewPrivate()
    {
        ;
    }

    HtmlPreview *q_ptr;

    MarkdownDocument *document;
    bool updateInProgress;
    bool updateAgain;
    PreviewProxy *proxy;
    QWebEngineView *view;
    QWebEngineProfile *profile;
    RemoteContentInterceptor *remoteContentInterceptor;
    KMessageWidget *remoteContentMessage;
    QAction *loadRemoteContentAction;
    QString baseUrl;
    QRegularExpression headingTagExp;
    Exporter *exporter;
    QString wrapperHtml;
    QFutureWatcher<QString> *futureWatcher;

    void onHtmlReady();
    void onLoadFinished(bool ok);
    void reloadWrapper();
    void showContextMenu(const QPoint &position);

    /**
     * Sets the base directory path for determining resource
     * paths relative to the web page being previewed.
     * This method is called whenever the file path changes.
     */
    void updateBaseDir();
    /*
     * Sets the HTML contents to display, and creates a backup of the old
     * HTML for diffing to scroll to the first difference whenever
     * updatePreview() is called.
     */
    void setHtmlContent(const QString &html);

    static QString exportToHtml(const QString &text, Exporter *exporter);
};

HtmlPreview::HtmlPreview(MarkdownDocument *document, Exporter *exporter, QWidget *parent)
    : QWidget(parent)
    , d_ptr(new HtmlPreviewPrivate(this))
{
    Q_D(HtmlPreview);

    d->document = document;
    d->updateInProgress = false;
    d->updateAgain = false;
    d->exporter = exporter;
    d->proxy->setMathEnabled(d->exporter->supportsMath());

    d->baseUrl = "";

    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    d->remoteContentMessage = new KMessageWidget(this);
    d->remoteContentMessage->setMessageType(KMessageWidget::Warning);
    d->remoteContentMessage->setPosition(KMessageWidget::Header);
    d->remoteContentMessage->setWordWrap(true);
    d->remoteContentMessage->setText(tr("External images and media were blocked to protect your privacy."));
    d->remoteContentMessage->setObjectName("remoteContentMessage");

    d->loadRemoteContentAction = new QAction(tr("Load for This Document"), d->remoteContentMessage);
    d->loadRemoteContentAction->setObjectName("loadRemoteContentAction");
    d->loadRemoteContentAction->setToolTip(tr("Allow external images and media until this document is closed"));
    d->remoteContentMessage->addAction(d->loadRemoteContentAction);
    d->remoteContentMessage->hide();
    layout->addWidget(d->remoteContentMessage);

    // Create the view before its private profile so QObject destroys the view
    // and page first. QWebEnginePage requires its profile to outlive it.
    d->view = new QWebEngineView(this);
    d->view->setObjectName("htmlPreviewView");
    layout->addWidget(d->view, 1);

    // An unnamed profile is off-the-record and cannot persist cookies, cache,
    // permissions, or other browsing data to disk.
    d->profile = new QWebEngineProfile(this);
    d->profile->setHttpCacheType(QWebEngineProfile::NoCache);
    d->profile->setPersistentCookiesPolicy(QWebEngineProfile::NoPersistentCookies);
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    d->profile->setPersistentPermissionsPolicy(QWebEngineProfile::PersistentPermissionsPolicy::AskEveryTime);
#endif
    d->remoteContentInterceptor = new RemoteContentInterceptor(d->profile);
    d->profile->setUrlRequestInterceptor(d->remoteContentInterceptor);

    SandboxedWebPage *webPage = new SandboxedWebPage(d->profile, d->view);
    d->view->setPage(webPage);

    d->view->settings()->setDefaultTextEncoding("utf-8");
    d->view->settings()->setAttribute(QWebEngineSettings::LocalContentCanAccessFileUrls, true);

    // The request interceptor enforces the remote-content policy. This setting
    // must remain enabled so it can observe blocked image and media requests
    // and offer the per-document opt-in.
    d->view->settings()->setAttribute(QWebEngineSettings::LocalContentCanAccessRemoteUrls, true);
    d->view->settings()->setAttribute(QWebEngineSettings::JavascriptCanOpenWindows, false);
    d->view->settings()->setAttribute(QWebEngineSettings::JavascriptCanAccessClipboard, false);
    d->view->settings()->setAttribute(QWebEngineSettings::LocalStorageEnabled, false);
    d->view->settings()->setAttribute(QWebEngineSettings::HyperlinkAuditingEnabled, false);
    d->view->settings()->setAttribute(QWebEngineSettings::PluginsEnabled, false);
    d->view->settings()->setAttribute(QWebEngineSettings::FullScreenSupportEnabled, false);
    d->view->settings()->setAttribute(QWebEngineSettings::ScreenCaptureEnabled, false);
    d->view->settings()->setAttribute(QWebEngineSettings::AllowRunningInsecureContent, false);
    d->view->settings()->setAttribute(QWebEngineSettings::AllowGeolocationOnInsecureOrigins, false);
    d->view->settings()->setAttribute(QWebEngineSettings::AllowWindowActivationFromJavaScript, false);
    d->view->settings()->setAttribute(QWebEngineSettings::PlaybackRequiresUserGesture, true);
    d->view->settings()->setAttribute(QWebEngineSettings::WebRTCPublicInterfacesOnly, true);
    d->view->settings()->setAttribute(QWebEngineSettings::DnsPrefetchEnabled, false);
    d->view->settings()->setAttribute(QWebEngineSettings::PdfViewerEnabled, false);
    d->view->settings()->setAttribute(QWebEngineSettings::NavigateOnDropEnabled, false);
    d->view->settings()->setUnknownUrlSchemePolicy(QWebEngineSettings::DisallowUnknownUrlSchemes);

    this->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    d->view->page()->action(QWebEnginePage::Reload)->setVisible(false);
    d->view->page()->action(QWebEnginePage::ReloadAndBypassCache)->setVisible(false);
    d->view->page()->action(QWebEnginePage::OpenLinkInThisWindow)->setVisible(false);
    d->view->page()->action(QWebEnginePage::OpenLinkInNewWindow)->setVisible(false);
    d->view->page()->action(QWebEnginePage::ViewSource)->setVisible(false);
    d->view->page()->action(QWebEnginePage::SavePage)->setVisible(false);

    this->connect(d->view, &QWebEngineView::loadFinished, [d](bool ok) {
        d->onLoadFinished(ok);
    });

    this->connect(d->view, &QWidget::customContextMenuRequested, [d](const QPoint &position) {
        d->showContextMenu(position);
    });
    d->view->setContextMenuPolicy(Qt::CustomContextMenu);

    this->connect(d->loadRemoteContentAction, &QAction::triggered, this, &HtmlPreview::allowRemoteContent);

    this->connect(
        d->remoteContentInterceptor,
        &RemoteContentInterceptor::loadableRemoteContentBlocked,
        this,
        [d]() {
            if (d->remoteContentInterceptor->hasBlockedLoadableContent() && !d->remoteContentInterceptor->remoteContentAllowed()) {
                d->remoteContentMessage->animatedShow();
            }
        },
        Qt::QueuedConnection);

#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    this->connect(webPage, &QWebEnginePage::permissionRequested, webPage, [](QWebEnginePermission permission) {
        permission.deny();
    });
#else
    this->connect(webPage, &QWebEnginePage::featurePermissionRequested, webPage, [webPage](const QUrl &securityOrigin, QWebEnginePage::Feature feature) {
        webPage->setFeaturePermission(securityOrigin, feature, QWebEnginePage::PermissionDeniedByUser);
    });
#endif

    d->headingTagExp.setPattern("^[Hh][1-6]$");

    d->futureWatcher = new QFutureWatcher<QString>(this);
    this->connect(d->futureWatcher, &QFutureWatcher<QString>::finished, [d]() {
        d->onHtmlReady();
    });

    this->connect(document, &MarkdownDocument::filePathChanged, [d]() {
        d->updateBaseDir();
    });

    // Set zoom factor for Chromium browser to account for system DPI settings,
    // since Chromium assumes 96 DPI as a fixed resolution.
    //
    qreal horizontalDpi = QGuiApplication::primaryScreen()->logicalDotsPerInchX();
    d->view->setZoomFactor((horizontalDpi / 96.0));

    QWebChannel *channel = new QWebChannel(this);
    channel->registerObject(QStringLiteral("previewProxy"), d->proxy);
    d->view->page()->setWebChannel(channel);

    QFile wrapperHtmlFile(":/resources/preview.html");

    if (!wrapperHtmlFile.open(QFile::ReadOnly | QFile::Text)) {
        d->wrapperHtml = tr("Error loading resources/preview.html");
    } else {
        QTextStream stream(&wrapperHtmlFile);
        d->wrapperHtml = stream.readAll();
        wrapperHtmlFile.close();
    }

    // Set the base URL and load the preview using wrapperHtml above.
    d->updateBaseDir();
}

HtmlPreview::~HtmlPreview()
{
    Q_D(HtmlPreview);

    // Wait for thread to finish if in the middle of updating the preview.
    d->futureWatcher->waitForFinished();
}

void HtmlPreview::updatePreview()
{
    Q_D(HtmlPreview);

    if (d->updateInProgress) {
        d->updateAgain = true;
        return;
    }

    if (this->isVisible()) {
        // Some markdown processors don't handle empty text very well
        // and will err.  Thus, only pass in text from the document
        // into the markdown processor if the text isn't empty or null.
        //
        if (d->document->isEmpty()) {
            d->setHtmlContent("");
        } else if (nullptr != d->exporter) {
            QString text = d->document->toPlainText();

            if (!text.isNull() && !text.isEmpty()) {
                d->updateInProgress = true;
                QFuture<QString> future = QtConcurrent::run(&HtmlPreviewPrivate::exportToHtml, d->document->toPlainText(), d->exporter);
                d->futureWatcher->setFuture(future);
            }
        }
    }
}

void HtmlPreview::navigateToHeading(int headingSequenceNumber)
{
    Q_D(HtmlPreview);

    d->view->page()->runJavaScript(QString("scrollToHeading(%1);").arg(headingSequenceNumber));
}

void HtmlPreview::setHtmlExporter(Exporter *exporter)
{
    Q_D(HtmlPreview);

    d->exporter = exporter;
    d->setHtmlContent("");
    d->proxy->setMathEnabled(d->exporter->supportsMath());
    updatePreview();
}

void HtmlPreview::setStyleSheet(const QString &css)
{
    Q_D(HtmlPreview);

    d->proxy->setStyleSheet(css);
}

void HtmlPreview::setMathEnabled(bool enabled)
{
    Q_D(HtmlPreview);

    d->proxy->setMathEnabled(enabled);
}

void HtmlPreview::allowRemoteContent()
{
    Q_D(HtmlPreview);

    d->remoteContentInterceptor->setRemoteContentAllowed(true);
    d->remoteContentMessage->animatedHide();
    d->reloadWrapper();
}

void HtmlPreview::resetRemoteContentPermission()
{
    Q_D(HtmlPreview);

    d->remoteContentInterceptor->setRemoteContentAllowed(false);
    d->remoteContentMessage->hide();
}

void HtmlPreviewPrivate::onHtmlReady()
{
    Q_Q(HtmlPreview);

    setHtmlContent(futureWatcher->result());
    updateInProgress = false;

    if (updateAgain) {
        updateAgain = false;
        q->updatePreview();
    }
}

void HtmlPreviewPrivate::onLoadFinished(bool ok)
{
    if (ok) {
        view->page()->runJavaScript("document.documentElement.contentEditable = false;");
    }
}

void HtmlPreviewPrivate::reloadWrapper()
{
    view->setHtml(wrapperHtml, QUrl(baseUrl));
}

void HtmlPreviewPrivate::showContextMenu(const QPoint &position)
{
    QMenu *menu = view->createStandardContextMenu();

    if (remoteContentInterceptor->hasBlockedLoadableContent() && !remoteContentInterceptor->remoteContentAllowed()) {
        menu->addSeparator();
        menu->addAction(loadRemoteContentAction);
    }

    QObject::connect(menu, &QMenu::aboutToHide, menu, &QObject::deleteLater);
    menu->popup(view->mapToGlobal(position));
}

void HtmlPreviewPrivate::updateBaseDir()
{
    Q_Q(HtmlPreview);

    if (!document->filePath().isNull() && !document->filePath().isEmpty()) {
        // Note that a forward slash ("/") is appended to the path to
        // ensure it works.  If the slash isn't there, then it won't
        // recognize the base URL for some reason.
        //
        baseUrl = QUrl::fromLocalFile(QFileInfo(document->filePath()).dir().absolutePath() + "/").toString();
    } else {
        this->baseUrl = "";
    }

    reloadWrapper();
    q->updatePreview();
}

void HtmlPreview::closeEvent(QCloseEvent *event)
{
    Q_UNUSED(event);
    Q_D(HtmlPreview);

    d->setHtmlContent("");
}

void HtmlPreviewPrivate::setHtmlContent(const QString &html)
{
    this->proxy->setHtmlContent(html);
}

QString HtmlPreviewPrivate::exportToHtml(const QString &text, Exporter *exporter)
{
    QString html;

    // Enable smart typography for preview, if available for the exporter.
    bool smartTypographyEnabled = exporter->smartTypographyEnabled();
    exporter->setSmartTypographyEnabled(true);

    // Export to HTML.
    exporter->exportToHtml(text, html);

    // Put smart typography setting back to the way it was before
    // so that the last setting used during document export is remembered.
    //
    exporter->setSmartTypographyEnabled(smartTypographyEnabled);

    return html;
}
} // namespace ghostwriter
