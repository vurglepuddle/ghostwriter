/*
 * SPDX-FileCopyrightText: 2018-2023 Megan Conkle <megan.conkle@kdemail.net>
 * SPDX-FileCopyrightText: 2026 ghostwriter contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef SANDBOXEDWEBPAGE_H
#define SANDBOXEDWEBPAGE_H

#include <QWebEnginePage>

class QWebEngineProfile;

namespace ghostwriter
{
/**
 * Web page for use with QWebEngineView that is "sandboxed" such that
 * external links cannot be visited without launching the default system
 * browser.
 */
class SandboxedWebPage : public QWebEnginePage
{
public:
    /**
     * Constructor.
     */
    SandboxedWebPage(QWebEngineProfile *profile, QObject *parent = nullptr);

    /**
     * Destructor.
     */
    virtual ~SandboxedWebPage();

    /**
     * Handles link clicks and opens external links with the
     * default system browser.
     */
    bool acceptNavigationRequest(const QUrl &url, QWebEnginePage::NavigationType type, bool isMainFrame) override;
};
} // namespace ghostwriter

#endif // SANDBOXEDWEBPAGE_H
