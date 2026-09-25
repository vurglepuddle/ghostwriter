# SPDX-FileCopyrightText: 2026 ghostwriter contributors
#
# SPDX-License-Identifier: GPL-3.0-or-later

cmake_minimum_required(VERSION 3.16)

project(folderviewtest VERSION 1.0.0 LANGUAGES CXX)

find_package(Qt6 REQUIRED COMPONENTS Core Test Widgets)

qt_standard_project_setup()

ecm_add_test(
    folderviewtest.cpp
    ../../src/bookmark.cpp
    ../../src/folderviewwidget.cpp
    ../../src/library.cpp
    LINK_LIBRARIES
        PRIVATE
            Qt::Core
            Qt::Test
            Qt::Widgets
    TEST_NAME folderviewtest
)

target_include_directories(folderviewtest PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
)

set_tests_properties(folderviewtest PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

enable_testing(true)
