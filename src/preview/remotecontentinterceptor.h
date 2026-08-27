/*
 * SPDX-FileCopyrightText: 2026 ghostwriter contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef REMOTE_CONTENT_INTERCEPTOR_H
#define REMOTE_CONTENT_INTERCEPTOR_H

#include <atomic>

#include <QUrl>
#include <QWebEngineUrlRequestInfo>
#include <QWebEngineUrlRequestInterceptor>

namespace ghostwriter
{
/**
 * Blocks network requests made by the live preview.
 *
 * Remote images and media can be enabled temporarily for the current
 * document. Active remote content remains blocked regardless of that setting.
 */
class RemoteContentInterceptor : public QWebEngineUrlRequestInterceptor
{
    Q_OBJECT

public:
    enum class RequestPolicy {
        Allow,
        Block,
        BlockAndNotify,
    };

    explicit RemoteContentInterceptor(QObject *parent = nullptr);

    void interceptRequest(QWebEngineUrlRequestInfo &info) override;

    void setRemoteContentAllowed(bool allowed);
    bool remoteContentAllowed() const;
    bool hasBlockedLoadableContent() const;

    static bool isRemoteUrl(const QUrl &url);
    static bool isLoadableRemoteResource(QWebEngineUrlRequestInfo::ResourceType resourceType);
    static RequestPolicy requestPolicy(const QUrl &url, QWebEngineUrlRequestInfo::ResourceType resourceType, bool remoteContentAllowed);

signals:
    void loadableRemoteContentBlocked();

private:
    std::atomic_bool m_remoteContentAllowed{false};
    std::atomic_bool m_blockedLoadableContent{false};
    std::atomic_bool m_blockNotificationSent{false};
};
} // namespace ghostwriter

#endif // REMOTE_CONTENT_INTERCEPTOR_H
