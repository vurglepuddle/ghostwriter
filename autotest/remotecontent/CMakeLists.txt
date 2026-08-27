# SPDX-FileCopyrightText: 2026 ghostwriter contributors
#
# SPDX-License-Identifier: GPL-3.0-or-later

cmake_minimum_required(VERSION 3.16)

project(remotecontentinterceptortest VERSION 1.0.0 LANGUAGES CXX)

find_package(Qt6 REQUIRED COMPONENTS Core Test WebEngineCore)

qt_standard_project_setup()

ecm_add_test(
    remotecontentinterceptortest.cpp
    ../../src/preview/remotecontentinterceptor.h
    ../../src/preview/remotecontentinterceptor.cpp
    LINK_LIBRARIES
        PRIVATE
            Qt::Core
            Qt::Test
            Qt::WebEngineCore
    TEST_NAME remotecontentinterceptortest
)

target_include_directories(remotecontentinterceptortest PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
)

enable_testing(true)
