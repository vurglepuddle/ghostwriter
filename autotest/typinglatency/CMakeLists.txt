# SPDX-FileCopyrightText: 2026 ghostwriter contributors
#
# SPDX-License-Identifier: GPL-3.0-or-later

cmake_minimum_required(VERSION 3.16)

project(typinglatencybench VERSION 1.0.0 LANGUAGES CXX)

find_package(Qt6 REQUIRED COMPONENTS Concurrent Core Gui Test Widgets)
find_package(KF6 REQUIRED COMPONENTS Sonnet)

qt_standard_project_setup()

if (MSVC)
    set(CMARK_TEST_LIBS
        libcmark-gfm
        libcmark-gfm-extensions
    )
else()
    set(CMARK_TEST_LIBS
        libcmark-gfm_static
        libcmark-gfm-extensions_static
    )
endif()

# A manual benchmark rather than a ctest test: it opens a visible window so
# that paint and text layout costs are real.
add_executable(typinglatencybench
    typinglatencybench.cpp
    ../../src/editor/actions/inlinemarkuptoggle.cpp
    ../../src/editor/markdowndocument.cpp
    ../../src/editor/markdowneditor.cpp
    ../../src/editor/markdownhighlighter.cpp
    ../../src/markdown/cmarkgfmapi.cpp
    ../../src/markdown/markdownast.cpp
    ../../src/markdown/markdownnode.cpp
    ../../src/markdown/memoryarena.cpp
    ../../src/messageboxhelper.cpp
    ../../src/outlinewidget.cpp
    ../../src/spelling/spellcheckdecorator.cpp
    ../../src/spelling/spellcheckdialog.cpp
    ../../src/statistics/documentstatistics.cpp
)

target_link_libraries(typinglatencybench PRIVATE
    Qt::Concurrent
    Qt::Core
    Qt::Gui
    Qt::Test
    Qt::Widgets
    KF6::SonnetCore
    KF6::SonnetUi
    ${CMARK_TEST_LIBS}
)

target_include_directories(typinglatencybench PRIVATE
    "${CMAKE_SOURCE_DIR}"
    "${CMAKE_SOURCE_DIR}/src"
    "${CMAKE_BINARY_DIR}/3rdparty/cmark-gfm/src"
)
