# SPDX-FileCopyrightText: 2026 ghostwriter contributors
#
# SPDX-License-Identifier: GPL-3.0-or-later

cmake_minimum_required(VERSION 3.16)

project(markdowneditortest VERSION 1.0.0 LANGUAGES CXX)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Test Widgets)

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

ecm_add_test(
    markdowneditortest.cpp
    ../../src/editor/actions/inlinemarkuptoggle.cpp
    ../../src/editor/markdowndocument.cpp
    ../../src/editor/markdowneditor.cpp
    ../../src/editor/markdownhighlighter.cpp
    ../../src/markdown/cmarkgfmapi.cpp
    ../../src/markdown/markdownast.cpp
    ../../src/markdown/markdownnode.cpp
    ../../src/markdown/memoryarena.cpp
    LINK_LIBRARIES
        PRIVATE
            Qt::Core
            Qt::Gui
            Qt::Test
            Qt::Widgets
            ${CMARK_TEST_LIBS}
    TEST_NAME markdowneditortest
)

target_include_directories(markdowneditortest PRIVATE
    "${CMAKE_SOURCE_DIR}"
    "${CMAKE_SOURCE_DIR}/src"
    "${CMAKE_BINARY_DIR}/3rdparty/cmark-gfm/src"
)

set_tests_properties(markdowneditortest PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

enable_testing(true)
