/*
 * SPDX-FileCopyrightText: 2021-2022 Megan Conkle <megan.conkle@kdemail.net>
 * SPDX-FileCopyrightText: 2026 ghostwriter contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// Live preview renderer.
//
// Each update parses the new HTML with the browser's own parser and then
// replaces only the top-level blocks that differ from what is displayed,
// so typing in one paragraph re-renders one paragraph rather than the whole
// document.  MathJax is loaded on demand, only when the current exporter
// supports math.

(function () {
    'use strict';

    var root = document.getElementById('livepreviewplaceholder');
    var lastHtml = '';
    var mathEnabled = false;
    var mathJaxState = 'unloaded'; // 'unloaded', 'loading' or 'ready'
    var pendingTypeset = [];

    // Source HTML of a displayed node, remembered at insertion time because
    // MathJax rewrites the DOM of nodes that contain math.
    function sourceOf(node) {
        if (node.gwSource !== undefined) {
            return node.gwSource;
        }

        return (1 === node.nodeType) ? node.outerHTML : node.nodeValue;
    }

    function loadStyleSheet(css) {
        var cssElem = document.getElementById('ghostwriter_css');

        if (!cssElem) {
            cssElem = document.createElement('style');
            cssElem.id = 'ghostwriter_css';
            cssElem.type = 'text/css';
            cssElem.media = 'all';
            document.head.appendChild(cssElem);
        }

        cssElem.textContent = css;
    }

    function typeset(elements) {
        if (!mathEnabled || (0 === elements.length)) {
            return;
        }

        if ('ready' === mathJaxState) {
            window.MathJax.typesetPromise(elements).catch(function (error) {
                console.error('MathJax typesetting failed: ' + error);
            });
            return;
        }

        pendingTypeset = pendingTypeset.concat(elements);

        if ('unloaded' === mathJaxState) {
            mathJaxState = 'loading';

            // window.MathJax holds the configuration from preview-init.js
            // until the library replaces it with its API.
            var config = window.MathJax || {};
            config.startup = config.startup || {};
            config.startup.typeset = false;
            config.startup.ready = function () {
                window.MathJax.startup.defaultReady();
                mathJaxState = 'ready';

                var elements = pendingTypeset.filter(function (element) {
                    return element.isConnected;
                });

                pendingTypeset = [];
                typeset(elements);
            };
            window.MathJax = config;

            var script = document.createElement('script');
            script.src = 'qrc:3rdparty/MathJax/bin/tex-svg-full.js';
            document.head.appendChild(script);
        }
    }

    function updateLivePreview(html) {
        lastHtml = html;

        var template = document.createElement('template');
        template.innerHTML = html;

        var newNodes = Array.prototype.slice.call(template.content.childNodes);
        var oldNodes = Array.prototype.slice.call(root.childNodes);
        var newSources = newNodes.map(function (node) {
            return (1 === node.nodeType) ? node.outerHTML : node.nodeValue;
        });

        // Skip the unchanged blocks at the beginning and end.
        var start = 0;

        while ((start < oldNodes.length)
                && (start < newNodes.length)
                && (sourceOf(oldNodes[start]) === newSources[start])) {
            start++;
        }

        var oldEnd = oldNodes.length - 1;
        var newEnd = newNodes.length - 1;

        while ((oldEnd >= start)
                && (newEnd >= start)
                && (sourceOf(oldNodes[oldEnd]) === newSources[newEnd])) {
            oldEnd--;
            newEnd--;
        }

        var reference = (oldEnd + 1 < oldNodes.length) ? oldNodes[oldEnd + 1] : null;

        for (var i = start; i <= oldEnd; i++) {
            root.removeChild(oldNodes[i]);
        }

        var inserted = [];

        for (var j = start; j <= newEnd; j++) {
            var node = newNodes[j];
            node.gwSource = newSources[j];
            root.insertBefore(node, reference);

            if (1 === node.nodeType) {
                inserted.push(node);
            }
        }

        // Bring the change into view, as the user is presumably looking at it
        // in the editor, but don't move if it is already visible.
        var changed = (inserted.length > 0)
            ? inserted[0]
            : ((start > 0) ? oldNodes[start - 1] : null);

        while (changed && (1 !== changed.nodeType)) {
            changed = changed.previousSibling;
        }

        if (changed && ((oldEnd >= start) || (newEnd >= start))) {
            changed.scrollIntoView({ block: 'nearest' });
        }

        typeset(inserted);
    }

    function setMathEnabled(enabled) {
        if (mathEnabled === enabled) {
            return;
        }

        mathEnabled = enabled;

        // Re-render everything so that math is either typeset or shown as
        // its source again.
        var html = lastHtml;
        root.textContent = '';
        updateLivePreview(html);
    }

    new QWebChannel(qt.webChannelTransport, function (channel) {
        var proxy = channel.objects.previewProxy;

        loadStyleSheet(proxy.styleSheet);
        proxy.styleSheetChanged.connect(loadStyleSheet);

        mathEnabled = proxy.mathEnabled;
        root.textContent = '';
        updateLivePreview(proxy.htmlContent);
        proxy.htmlChanged.connect(updateLivePreview);
        proxy.mathToggled.connect(setMathEnabled);
    });
})();
