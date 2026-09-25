# <img src="./resources/icons/ghostwriter.png" align="left" width="44" style="padding-right:5px"> ghostwriter

This is a personal fork of [*ghostwriter*](https://invent.kde.org/office/ghostwriter), the distraction-free Markdown editor from KDE. It is tuned for writing long prose on Windows: typing stays instant in large documents, the app opens quickly, and there are a few writing tools the original doesn't have.

For general help with *ghostwriter* itself, see the [quick reference guide](https://ghostwriter.kde.org/documentation/) on the original project's site.

## What this fork changes

### Speed

* **Typing is instant, even in big documents.** Typing, deleting and pasting take a few milliseconds no matter how long the document is. In a 70 KB document, typing used to fall seconds behind. The editor now reformats only the lines that actually changed, and parses Markdown in the background.
* **The app opens fast.** The window appears in about 0.6 seconds instead of about 1 second (measured on a Windows machine with a 43 KB document), and your last document shows up right after. Anything the document doesn't need to be shown, like the spell checker's dictionaries, the folder view and Live Preview, loads once the document is on screen.
* **Files are loaded once.** Opening a file from another folder, including the last file at startup, used to load it twice.
* **Spell checking stays out of the way.** It runs only when you pause typing, checks only the paragraphs that changed, and remembers words it has already checked. It picks the dictionary by the writing system of the text (Latin, Cyrillic and so on), and only guesses the language when you have dictionaries for several languages that share one.
* **Statistics and the outline don't slow you down.** Word counts are updated for changed paragraphs once you pause, and the outline isn't rebuilt while it's hidden.
* **Live Preview costs nothing until you use it.** Its browser engine only starts when Preview is turned on. The preview then updates only the parts of the page that changed, a moment after you stop typing, and loads math support only when the selected Markdown processor supports math.
* **External Markdown processors are found when needed.** Pandoc, MultiMarkdown and cmark are looked for the first time you need them, not every time the app starts.

### New features

* **Blind Draft Mode** (View menu, or `Ctrl+Shift+B`). While it's on, you only see and edit the line you are writing. Everything before it is locked, so you keep moving forward instead of rewriting. Turn it off to see and edit the whole document again.
* **Safer Live Preview.** Images and media from the internet are blocked by default. A bar at the top of the preview lets you load them for the current document when you want them. Scripts inside documents never run, and the preview uses a private browser profile that doesn't keep anything.

### Fixes

* Opening Markdown files from Windows Explorer with "Open with" works.
* The Windows app has its own icon.
* The folder view follows the light or dark theme.
* Resizing the window no longer lags, and the sidebar no longer flashes white when it hides or reappears.

The full list is in [CHANGELOG.md](CHANGELOG.md).

## Getting the app

There are no ready-made downloads of this fork. Build it from source as described below.

If you want the original *ghostwriter* instead, KDE packages it for Linux, and there are more options on the [original project's site](https://ghostwriter.kde.org).

## Building on Windows

This fork is built on Windows with Visual Studio 2022, Qt 6.11 and KDE Frameworks 6.29, with Qt and the Frameworks installed through [KDE Craft](https://community.kde.org/Craft).

1. Install Visual Studio 2022 with the "Desktop development with C++" workload.
2. Install KDE Craft (these steps assume it's in `C:\CraftRoot`). Use it to install:
   * Qt 6, including Qt SVG, Qt WebEngine and Qt WebChannel
   * the KDE Frameworks CoreAddons, ConfigWidgets, WidgetsAddons, XmlGui and Sonnet
   * Extra CMake Modules
3. Open an "x64 Native Tools Command Prompt for VS 2022", add Craft's tools to your `PATH`, and build:

        set PATH=C:\CraftRoot\bin;C:\CraftRoot\dev-utils\bin;%PATH%
        cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=C:/CraftRoot -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl
        cmake --build build-release

   Keep the two `cl` settings. Without them, CMake picks Craft's own compiler, and the build fails.
4. The app is `build-release\bin\ghostwriter.exe`. To run it, `C:\CraftRoot\bin` needs to be on your `PATH`, and a file named `qt.conf` next to the exe tells Qt where Craft keeps its plugins:

        [Paths]
        Prefix = C:/CraftRoot
        Binaries = C:/CraftRoot/bin
        Libraries = C:/CraftRoot/lib
        LibraryExecutables = C:/CraftRoot/bin
        Plugins = C:/CraftRoot/plugins
        QmlImports = C:/CraftRoot/qml
        Data = C:/CraftRoot/bin
        Translations = C:/CraftRoot/translations

5. To run the tests, use `ctest --test-dir build-release`.

### Other systems

This fork doesn't add any dependencies, so the [original build instructions](https://invent.kde.org/office/ghostwriter) for Linux, macOS and FreeBSD should still work. They haven't been tested with this fork.

## Command line

You can open a file straight from a terminal:

    ghostwriter myfile.md

If the file doesn't exist yet, *ghostwriter* creates it.

To turn off GPU acceleration, add `--disable-gpu`:

    ghostwriter --disable-gpu

This can help on Windows if menus don't show up in full screen mode while Live Preview is on.

## Other Markdown processors

*ghostwriter* has the cmark-gfm processor built in. It can also use Pandoc, MultiMarkdown or cmark if you install them and make sure they are on your `PATH`. They then show up as options for Live Preview and exporting.

## Credits and license

*ghostwriter* was created by Megan Conkle (wereturtle) and is developed by KDE. This fork is built on their work.

The source code is licensed under the [GNU General Public License version 3](http://www.gnu.org/licenses/gpl.html). Some icons and third-party code (such as cmark-gfm and MathJax) use other licenses that are compatible with it. See the COPYING and LICENSE files in the respective folders for details.
