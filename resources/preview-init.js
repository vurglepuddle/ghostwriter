/*
 * SPDX-FileCopyrightText: 2021-2022 Megan Conkle <megan.conkle@kdemail.net>
 * SPDX-FileCopyrightText: 2026 ghostwriter contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

window.MathJax = {
    tex: {
        inlineMath: [['$', '$'], ['\\(', '\\)'], ['\\[', '\\]']]
    }
};

function scrollToHeading(headingNumber) {
    var headers = document.querySelectorAll(
        'div > h1, div > h2, div > h3, div > h4, div > h5, div > h6');

    if ((headingNumber > 0) && (headingNumber <= headers.length)) {
        headers[headingNumber - 1].scrollIntoView();
    }
}
