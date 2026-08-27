/*
 * SPDX-FileCopyrightText: 2026 ghostwriter contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "remotecontentinterceptor.h"

namespace ghostwriter
{
RemoteContentInterceptor::RemoteContentInterceptor(QObject *parent)
    : QWebEngineUrlRequestInterceptor(parent)
{
}

void RemoteContentInterceptor::interceptRequest(QWebEngineUrlRequestInfo &info)
{
    RequestPolicy policy = requestPolicy(info.requestUrl(), info.resourceType(), m_remoteContentAllowed.load());

    if (RequestPolicy::Allow == policy) {
        return;
    }

    info.block(true);

    if (RequestPolicy::BlockAndNotify == policy) {
        m_blockedLoadableContent.store(true);

        bool expected = false;

        if (m_blockNotificationSent.compare_exchange_strong(expected, true)) {
            emit loadableRemoteContentBlocked();
        }
    }
}

void RemoteContentInterceptor::setRemoteContentAllowed(bool allowed)
{
    m_remoteContentAllowed.store(allowed);
    m_blockedLoadableContent.store(false);
    m_blockNotificationSent.store(false);
}

bool RemoteContentInterceptor::remoteContentAllowed() const
{
    return m_remoteContentAllowed.load();
}

bool RemoteContentInterceptor::hasBlockedLoadableContent() const
{
    return m_blockedLoadableContent.load();
}

bool RemoteContentInterceptor::isRemoteUrl(const QUrl &url)
{
    const QString scheme = url.scheme().toLower();

    if (scheme.isEmpty()) {
        return false;
    }

    return ("file" != scheme) && ("qrc" != scheme) && ("data" != scheme) && ("blob" != scheme) && ("about" != scheme);
}

bool RemoteContentInterceptor::isLoadableRemoteResource(QWebEngineUrlRequestInfo::ResourceType resourceType)
{
    return (QWebEngineUrlRequestInfo::ResourceTypeImage == resourceType) || (QWebEngineUrlRequestInfo::ResourceTypeMedia == resourceType);
}

RemoteContentInterceptor::RequestPolicy
RemoteContentInterceptor::requestPolicy(const QUrl &url, QWebEngineUrlRequestInfo::ResourceType resourceType, bool remoteContentAllowed)
{
    if (!isRemoteUrl(url)) {
        return RequestPolicy::Allow;
    }

    if (!isLoadableRemoteResource(resourceType)) {
        return RequestPolicy::Block;
    }

    const QString scheme = url.scheme().toLower();

    if (("http" != scheme) && ("https" != scheme)) {
        return RequestPolicy::Block;
    }

    if (remoteContentAllowed) {
        return RequestPolicy::Allow;
    }

    return RequestPolicy::BlockAndNotify;
}
} // namespace ghostwriter
