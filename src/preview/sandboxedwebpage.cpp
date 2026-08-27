/*
 * SPDX-FileCopyrightText: 2018-2023 Megan Conkle <megan.conkle@kdemail.net>
 * SPDX-FileCopyrightText: 2026 ghostwriter contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <QDesktopServices>

#include "remotecontentinterceptor.h"
#include "sandboxedwebpage.h"

namespace ghostwriter
{
SandboxedWebPage::SandboxedWebPage(QWebEngineProfile *profile, QObject *parent)
    : QWebEnginePage(profile, parent)
{
}

SandboxedWebPage::~SandboxedWebPage()
{
    ;
}

bool SandboxedWebPage::acceptNavigationRequest(const QUrl &url, QWebEnginePage::NavigationType type, bool isMainFrame)
{
    if (QWebEnginePage::NavigationTypeLinkClicked == type) {
        const QString scheme = url.scheme().toLower();

        if (url.isLocalFile() || ("http" == scheme) || ("https" == scheme) || ("mailto" == scheme)) {
            QDesktopServices::openUrl(url);
        }

        return false;
    }

    // Prevent redirects, form submissions, and other document-supplied
    // navigation from replacing the local preview with a remote page.
    if (isMainFrame && RemoteContentInterceptor::isRemoteUrl(url)) {
        return false;
    }

    return true;
}
} // namespace ghostwriter
