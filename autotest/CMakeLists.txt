# SPDX-FileCopyrightText: 2022-2024 Megan Conkle <megan.conkle@kdemail.net>
# SPDX-FileCopyrightText: 2026 ghostwriter contributors
#
# SPDX-License-Identifier: GPL-3.0-or-later

cmake_minimum_required(VERSION 3.16)

project(unittest VERSION 1.0.0 LANGUAGES CXX)

add_subdirectory(asynctextwriter)
add_subdirectory(bookmark)
add_subdirectory(library)
add_subdirectory(markdowneditor)
add_subdirectory(remotecontent)

enable_testing(true)
