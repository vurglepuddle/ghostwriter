<!--
SPDX-FileCopyrightText: 2026 ghostwriter contributors
SPDX-License-Identifier: CC-BY-SA-4.0
-->

# Blind Draft Mode handoff

## Status

Blind Draft Mode is implemented as an independent, transient editor mode. The
implementation is complete, the application builds successfully, and the full
Qt test suite passes on Windows with MSVC 2022, Qt 6.11.1, and KDE Frameworks
6.29.0 from KDE Craft. Follow-up integration work also hardened Live Preview,
fixed Windows runtime deployment and executable icon handling, and corrected
sidebar/resizing theme artifacts discovered during manual testing.

## Implementation approach

- `MarkdownEditor` keeps the complete `QTextDocument` unchanged and marks every
  `QTextBlock` except the active newline-delimited block as not visible.
- Cursor and selection changes are synchronously clamped to the active block,
  covering keyboard, mouse, outline, search, and other programmatic navigation.
- Enter, keypad Enter, multiline paste/drop, and code-fence insertion advance
  the active block and commit all preceding blocks.
- Backspace at the active block start and Delete at its end are blocked so they
  cannot join committed or future hidden blocks.
- Undo uses a checkpoint recorded at enable/commit time. Redo tracks only the
  operations created by allowed current-line undos. Edits made on the current
  line can therefore be undone/redone, but a commit cannot be crossed until
  Blind Draft Mode is disabled. The standard context menu uses the same
  boundary.
- Document loading has an explicit guarded navigation path so the saved cursor
  line becomes active while Blind Draft Mode remains enabled.
- Find, Find Previous/Next, match highlighting, Replace, and Replace All are
  constrained to the active line. Whole-document spelling dialogs are disabled
  and any already-open spelling dialog is closed on enable.
- Live Preview is temporarily hidden and its prior visibility is restored on
  disable. Its persisted preference is not changed.
- The mode is exposed through the normal KDE action collection, View menu, and
  status bar. Default shortcut: `Ctrl+Shift+B`.
- The mode is not persisted, matching Hemingway and Distraction-Free mode
  conventions in this codebase.

## Exact behavior and interactions

| Blind Draft | Hemingway | Result |
|---|---|---|
| Off | Off | Normal editing and navigation. |
| On | Off | Only the current document block is visible/editable; Backspace and Delete work inside it. |
| Off | On | Existing Hemingway behavior remains unchanged. |
| On | On | Blind Draft visibility/navigation locks apply and Hemingway continues blocking Backspace/Delete. |

Focus Mode remains an independent setting. Its current-line/sentence/paragraph
formatting can stay enabled, but hidden Blind Draft blocks are still completely
suppressed. Disabling Blind Draft immediately restores all blocks; Focus Mode
then continues with its previously selected behavior.

Saving and autosaving still call `MarkdownDocument::toPlainText()`, so they save
all hidden and visible text. Toggling the mode only changes block visibility and
does not mark or modify the document.

## Edge-case coverage

The new `markdowneditortest` covers:

- enabling in the middle of an existing document and disabling again;
- unchanged underlying text and document modified state;
- empty documents, blank lines, and consecutive Enter/keypad Enter presses;
- hard document blocks versus soft-wrapped visual rows;
- blocked keyboard, mouse, programmatic, and cross-line selection navigation;
- Backspace/Delete inside the current line and at both protected boundaries;
- Select All, Copy, multiline Paste, and paste commit behavior;
- current-line undo/redo and the post-commit undo boundary;
- loading another document at a stored cursor line while enabled;
- Focus Mode and Hemingway Mode enabled together with Blind Draft Mode.

No known in-application editor path can place the caret or an editable selection
in a hidden block.

## Follow-up integration work

### Live Preview hardening

- Live Preview now uses an unnamed, off-the-record `QWebEngineProfile` with no
  persistent cookies or HTTP cache and denies feature permissions.
- A request interceptor permanently blocks remote scripts, stylesheets,
  subframes, XHR, WebSockets, pings, service workers, custom schemes, and other
  active content.
- External HTTP(S) images and media are blocked by default. A warning bar can
  load those passive resources for the current document only; consent resets
  when a document is loaded or closed.
- The preview document has a restrictive Content Security Policy. Former inline
  JavaScript was moved to bundled `qrc:` resources.
- Main-frame navigation is limited to supported local/file, HTTP(S), and mail
  links initiated by the user. Unknown protocols are denied.
- `remotecontentinterceptortest` covers the network-request policy.

### Windows build and UI fixes

- `qt.conf` beside the local Release executable points this development build
  at the KDE Craft runtime, plugins, QML, translations, and Qt WebEngine data.
  It is an ignored, machine-local build artifact and is not committed.
- The Windows RC language is explicitly enabled. The custom application icon
  is compiled into the PE executable with native 16, 24, 32, 48, 64, 128, and
  256 px entries.
- Folder View (`QTreeView`) colors now use the active ghostwriter theme instead
  of the Windows default white view palette.
- `MainWindow::resizeEvent()` now calls the required `QMainWindow` base handler,
  ensuring the central splitter and status bar track the native client size.
- Editor adjustment no longer re-enters the complete Qt event loop during a
  partially processed resize. The main window and splitter also have explicit
  theme backgrounds, preventing white flashes as the sidebar auto-hides.
- Interactive resize events now coalesce the comparatively expensive editor
  margin and cursor-centering work on a short single-shot timer. This reduces
  the time between a native Windows client resize and the next Qt backing-store
  paint when the top or left edge moves the window origin; sidebar visibility
  changes use the same deferred path.
- A very brief uncovered strip can still appear during sufficiently rapid
  top/left-edge resizing. The same behavior was reproduced in unrelated native
  applications on this machine, identifying the residual as Windows/DWM frame
  composition outrunning an application's next rendered buffer rather than a
  ghostwriter animation or layout defect. No native window-procedure workaround
  was added because it would be platform-specific and riskier than the cosmetic
  system behavior.

## Files changed

- `src/editor/markdowneditor.h`
- `src/editor/markdowneditor.cpp`
- `src/appactions.h`
- `src/appactions.cpp`
- `src/mainwindow.h`
- `src/mainwindow.cpp`
- `src/documentmanager.cpp`
- `src/findreplace.cpp`
- `src/spelling/spellcheckdecorator.cpp`
- `src/preview/htmlpreview.h`
- `src/preview/htmlpreview.cpp`
- `src/preview/sandboxedwebpage.h`
- `src/preview/sandboxedwebpage.cpp`
- `src/preview/remotecontentinterceptor.h`
- `src/preview/remotecontentinterceptor.cpp`
- `resources/preview.html`
- `resources/preview-init.js`
- `resources/preview.js`
- `resources/widgets.qss`
- `resources/windows/ghostwriter.ico`
- `resources/windows/ghostwriter.rc`
- `resources.qrc`
- `CMakeLists.txt`
- `src/CMakeLists.txt`
- `autotest/CMakeLists.txt`
- `autotest/markdowneditor/CMakeLists.txt`
- `autotest/markdowneditor/markdowneditortest.cpp`
- `autotest/remotecontent/CMakeLists.txt`
- `autotest/remotecontent/remotecontentinterceptortest.cpp`
- `doc/man-ghostwriter.1.docbook`
- `CHANGELOG.md`
- `.reuse/dep5`
- `HANDOFF_BLIND_DRAFT_MODE.md`

## Verification performed

- The Release tree configured successfully against `C:/CraftRoot` from the
  Visual Studio 2022 x64 developer environment.
- `cmake --build build-release` built the complete application, Windows icon
  resource, documentation, and all tests.
- `ctest --test-dir build-release --output-on-failure -C Release` passed all
  five test targets: `asynctextwritertest`, `bookmarktest`, `librarytest`,
  `markdowneditortest`, and `remotecontentinterceptortest`.
- The rebuilt executable's extracted icon matched the generated source ICO
  pixel-for-pixel at 32 px.
- An automated GUI diagnostic performed 121 rapid horizontal resize operations
  across the sidebar auto-hide threshold. The captured final frame had the
  sidebar correctly hidden, the editor filling the client area, and the status
  bar anchored to the bottom without an exposed band.
- A directional capture reproduced the artifact as a stale client backing image
  while the native frame changed size. Manual testing confirmed that the
  app-side changes make it disappear faster, while comparison with unrelated
  applications confirmed that the remaining brief strip is system-wide
  Windows/DWM resize behavior.
- `reuse lint-file` passed for the changed icon, RC, CMake, stylesheet,
  changelog, and handoff files.
- The repository-wide `reuse lint` still reports pre-existing compliance issues:
  a missing `LGPL-3.0-or-later` license text, several unrelated files without
  metadata, and generated files under local build directories.
- The `dev` preset compiles the feature sources, but its Debug targets cannot be
  linked reliably against Craft's release Qt binaries. The Release preset
  matches the installed Craft ABI and was used for definitive verification.
- `git diff --check` passed.

To reproduce the verified build in this KDE Craft environment, run from a
Visual Studio Developer PowerShell:

```powershell
$craftRoot = "C:\CraftRoot"
$buildDirectory = Join-Path (Get-Location) "build-release"
$env:Path = "$craftRoot\bin;$env:Path"
& "$craftRoot\dev-utils\bin\cmake.exe" --build $buildDirectory --parallel
& "$craftRoot\dev-utils\bin\ctest.exe" --test-dir $buildDirectory --output-on-failure -C Release --timeout 60
```
