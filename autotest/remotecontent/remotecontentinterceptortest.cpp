/*
 * SPDX-FileCopyrightText: 2026 ghostwriter contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <QTest>

#include "preview/remotecontentinterceptor.h"

using namespace ghostwriter;

class RemoteContentInterceptorTest : public QObject
{
    Q_OBJECT

private slots:
    void identifiesRemoteSchemes_data();
    void identifiesRemoteSchemes();
    void permitsLocalPreviewResources();
    void requiresConsentForRemoteImagesAndMedia_data();
    void requiresConsentForRemoteImagesAndMedia();
    void rejectsNonHttpRemoteImages();
    void alwaysBlocksActiveRemoteContent_data();
    void alwaysBlocksActiveRemoteContent();
    void resetsPerDocumentPermission();
};

void RemoteContentInterceptorTest::identifiesRemoteSchemes_data()
{
    QTest::addColumn<QUrl>("url");

    QTest::newRow("http") << QUrl("http://example.test/image.png");
    QTest::newRow("https") << QUrl("https://example.test/image.png");
    QTest::newRow("ftp") << QUrl("ftp://example.test/image.png");
    QTest::newRow("websocket") << QUrl("ws://example.test/socket");
    QTest::newRow("secure-websocket") << QUrl("wss://example.test/socket");
    QTest::newRow("custom-protocol") << QUrl("custom:payload");
}

void RemoteContentInterceptorTest::identifiesRemoteSchemes()
{
    QFETCH(QUrl, url);

    QVERIFY(RemoteContentInterceptor::isRemoteUrl(url));
}

void RemoteContentInterceptorTest::permitsLocalPreviewResources()
{
    using Info = QWebEngineUrlRequestInfo;
    using Policy = RemoteContentInterceptor::RequestPolicy;

    const QList<QUrl> localUrls = {
        QUrl("file:///tmp/image.png"),
        QUrl("qrc:/resources/preview.html"),
        QUrl("data:image/png;base64,AA=="),
        QUrl("blob:local-preview-resource"),
        QUrl("about:blank"),
    };

    for (const QUrl &url : localUrls) {
        QCOMPARE(RemoteContentInterceptor::requestPolicy(url, Info::ResourceTypeImage, false), Policy::Allow);
    }
}

void RemoteContentInterceptorTest::requiresConsentForRemoteImagesAndMedia_data()
{
    QTest::addColumn<int>("resourceType");

    QTest::newRow("image") << static_cast<int>(QWebEngineUrlRequestInfo::ResourceTypeImage);
    QTest::newRow("media") << static_cast<int>(QWebEngineUrlRequestInfo::ResourceTypeMedia);
}

void RemoteContentInterceptorTest::requiresConsentForRemoteImagesAndMedia()
{
    using Info = QWebEngineUrlRequestInfo;
    using Policy = RemoteContentInterceptor::RequestPolicy;

    QFETCH(int, resourceType);
    auto type = static_cast<Info::ResourceType>(resourceType);
    const QUrl url("https://example.test/content");

    QCOMPARE(RemoteContentInterceptor::requestPolicy(url, type, false), Policy::BlockAndNotify);
    QCOMPARE(RemoteContentInterceptor::requestPolicy(url, type, true), Policy::Allow);
}

void RemoteContentInterceptorTest::rejectsNonHttpRemoteImages()
{
    using Info = QWebEngineUrlRequestInfo;
    using Policy = RemoteContentInterceptor::RequestPolicy;

    const QList<QUrl> urls = {
        QUrl("ftp://example.test/image.png"),
        QUrl("custom:image"),
        QUrl("javascript:alert(1)"),
    };

    for (const QUrl &url : urls) {
        QCOMPARE(RemoteContentInterceptor::requestPolicy(url, Info::ResourceTypeImage, true), Policy::Block);
    }
}

void RemoteContentInterceptorTest::alwaysBlocksActiveRemoteContent_data()
{
    QTest::addColumn<int>("resourceType");

    QTest::newRow("script") << static_cast<int>(QWebEngineUrlRequestInfo::ResourceTypeScript);
    QTest::newRow("stylesheet") << static_cast<int>(QWebEngineUrlRequestInfo::ResourceTypeStylesheet);
    QTest::newRow("sub-frame") << static_cast<int>(QWebEngineUrlRequestInfo::ResourceTypeSubFrame);
    QTest::newRow("xhr") << static_cast<int>(QWebEngineUrlRequestInfo::ResourceTypeXhr);
    QTest::newRow("websocket") << static_cast<int>(QWebEngineUrlRequestInfo::ResourceTypeWebSocket);
    QTest::newRow("tracking-ping") << static_cast<int>(QWebEngineUrlRequestInfo::ResourceTypePing);
    QTest::newRow("service-worker") << static_cast<int>(QWebEngineUrlRequestInfo::ResourceTypeServiceWorker);
}

void RemoteContentInterceptorTest::alwaysBlocksActiveRemoteContent()
{
    using Info = QWebEngineUrlRequestInfo;
    using Policy = RemoteContentInterceptor::RequestPolicy;

    QFETCH(int, resourceType);
    auto type = static_cast<Info::ResourceType>(resourceType);
    const QUrl url("https://example.test/active-content");

    QCOMPARE(RemoteContentInterceptor::requestPolicy(url, type, false), Policy::Block);
    QCOMPARE(RemoteContentInterceptor::requestPolicy(url, type, true), Policy::Block);
}

void RemoteContentInterceptorTest::resetsPerDocumentPermission()
{
    RemoteContentInterceptor interceptor;

    QVERIFY(!interceptor.remoteContentAllowed());
    interceptor.setRemoteContentAllowed(true);
    QVERIFY(interceptor.remoteContentAllowed());
    interceptor.setRemoteContentAllowed(false);
    QVERIFY(!interceptor.remoteContentAllowed());
}

QTEST_GUILESS_MAIN(RemoteContentInterceptorTest)

#include "remotecontentinterceptortest.moc"
