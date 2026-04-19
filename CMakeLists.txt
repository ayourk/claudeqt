# SPDX-License-Identifier: GPL-3.0-only
#
# Top-level CMakeLists. The version range, dependency probes, and
# Qt6+KF5-qt6 fallback order are tuned for Ubuntu Noble's stock
# Qt 6.4.2 plus the kf5syntaxhighlighting-qt6 backport shipped via
# ppa:ayourk/claudeqt.
#
# Floor 3.22 = Jammy stock (22.04 LTS), so future Jammy ports don't
# need a newer CMake. Upper 4.3 enables modern policy defaults under
# CMake 4.x (e.g. a future Resolute chroot) without requiring it.
#
# Fork hook: the project() declaration below is the single place a
# downstream fork renames. Every target name, compile define, and
# install path downstream is derived from ${PROJECT_NAME} so that
# one edit re-themes the whole build. Build options use the neutral
# APP_ prefix (APP_SYSTEM_INTEGRATION, APP_BUILD_REMOTE, BUILD_TESTS)
# so they carry no brand and downstream packaging can drive them
# without a name that has to change with every fork rename.
cmake_minimum_required(VERSION 3.22...4.3)

macro(project_details _name)
    cmake_parse_arguments(_PD "" "ISSUES_URL;PPA" "" ${ARGN})
    project(${_name} ${_PD_UNPARSED_ARGUMENTS})
    if(_PD_ISSUES_URL)
        string(REPLACE "@HOMEPAGE_URL@" "${PROJECT_HOMEPAGE_URL}" PROJECT_ISSUES_URL "${_PD_ISSUES_URL}")
    else()
        set(PROJECT_ISSUES_URL "${PROJECT_HOMEPAGE_URL}/issues")
    endif()
    if(_PD_PPA)
        set(PROJECT_PPA "${_PD_PPA}")
    endif()
endmacro()

project_details(claudeqt
    VERSION 0.0.1
    DESCRIPTION "Native Qt6 GUI for Claude Code CLI"
    HOMEPAGE_URL "https://github.com/ayourk/claudeqt"
    ISSUES_URL "@HOMEPAGE_URL@/issues"
    PPA "ppa:ayourk/claudeqt"
    LANGUAGES C CXX)

string(TOUPPER "${PROJECT_NAME}" PROJECT_NAME_UPPER)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

option(APP_SYSTEM_INTEGRATION
    "Install systemd units, desktop entry, and icons to FHS system paths" OFF)
option(APP_BUILD_REMOTE
    "Build the claudeqt-remote daemon (Linux only)" ON)
# -DBUILD_TESTS=ON wires in the QtTest suite under tests/. Default
# ON for dev builds; packagers flip it off for release tarballs if
# they don't want the test binaries in the source package build.
option(BUILD_TESTS
    "Build the QtTest unit test suite" ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

include(GNUInstallDirs)

# --- APP_VERSION from debian/changelog (see cmake/AppVersion.cmake) ---
include(AppVersion)
app_read_version(APP_VERSION)

# --- Qt6 probe (cctv-viewer pattern; 6.4.2 floor matches Noble stock) ---
find_package(Qt6 6.4.2 REQUIRED COMPONENTS
    Core Gui Widgets Svg Network Sql LinguistTools)

# --- KSyntaxHighlighting: try KF6 first, fall back to KF5-qt6 ---
#
# The KF5-qt6 backport (see packaging/ppa/kf5syntaxhighlighting-qt6/)
# installs under the R6-isolated tree /usr/lib/<arch>/kf5-qt6/ rather
# than the stock KDE path, to avoid colliding with Ubuntu's stock
# Qt5 KF5 packages. The HINTS below points at that isolated CMake
# config tree. KF6 builds on distributions that ship it pick up via
# the first branch and never touch the hint.
#
# APP_USE_KSYNTAXHIGHLIGHTING gate exists for cross-compilation (MXE/Windows)
# where KSH isn't packaged. When OFF, the editor falls back to plain
# QPlainTextEdit without syntax highlighting.
option(APP_USE_KSYNTAXHIGHLIGHTING
    "Enable KSyntaxHighlighting-based code editor" ON)
set(APP_KSH_TARGET "")
if(APP_USE_KSYNTAXHIGHLIGHTING)
    find_package(KF6SyntaxHighlighting QUIET)
    if(KF6SyntaxHighlighting_FOUND)
        set(APP_KSH_TARGET KF6::SyntaxHighlighting)
        message(STATUS "Using KF6 SyntaxHighlighting ${KF6SyntaxHighlighting_VERSION}")
    else()
        find_package(KF5SyntaxHighlighting 5.115 REQUIRED
            HINTS /usr/lib/${CMAKE_LIBRARY_ARCHITECTURE}/kf5-qt6/cmake/KF5SyntaxHighlighting
                  /usr/lib/x86_64-linux-gnu/kf5-qt6/cmake/KF5SyntaxHighlighting
                  /usr/lib/aarch64-linux-gnu/kf5-qt6/cmake/KF5SyntaxHighlighting)
        set(APP_KSH_TARGET KF5::SyntaxHighlighting)
        message(STATUS "Using KF5 SyntaxHighlighting Qt6 backport ${KF5SyntaxHighlighting_VERSION}")
    endif()
else()
    message(STATUS "KSyntaxHighlighting disabled — editor will use plain QPlainTextEdit")
endif()

# --- cmark-gfm via pkg-config ---
#
# Noble's libcmark-gfm-dev ships libcmark-gfm.pc, but libcmark-gfm-extensions-dev
# does NOT provide a .pc file — it only installs the .so and headers. So we
# probe the core module via pkg-config and locate the extensions library by hand.
#
# APP_USE_CMARK_GFM gate exists for cross-compilation (MXE/Windows) where
# cmark-gfm isn't packaged. When OFF, chat messages render as plain text.
option(APP_USE_CMARK_GFM
    "Enable cmark-gfm markdown rendering for chat messages" ON)
if(UNIX)
    find_package(PkgConfig REQUIRED)
endif()
if(APP_USE_CMARK_GFM)
    pkg_check_modules(CMARK_GFM REQUIRED IMPORTED_TARGET libcmark-gfm)
    find_library(CMARK_GFM_EXTENSIONS_LIBRARY
        NAMES cmark-gfm-extensions
        REQUIRED)
    find_path(CMARK_GFM_EXTENSIONS_INCLUDE_DIR
        NAMES cmark-gfm-core-extensions.h
        REQUIRED)
else()
    message(STATUS "cmark-gfm disabled — chat messages will render as plain text")
endif()

# --- SQLite (Qt6::Sql links it, but we need the C headers for PRAGMAs) ---
find_package(SQLite3 3.38 REQUIRED)

# --- libsystemd for sd_notify() in claudeqt-remote (Linux only) ---
if(UNIX AND NOT APPLE AND APP_BUILD_REMOTE)
    pkg_check_modules(SYSTEMD REQUIRED IMPORTED_TARGET libsystemd)
endif()

# --- Python embedding (future feature) ---
option(APP_EMBED_PYTHON "Embed Python interpreter (future feature)" OFF)
if(APP_EMBED_PYTHON)
    find_package(Python3 3.12 REQUIRED COMPONENTS Development Interpreter)
endif()

add_subdirectory(src/persistence)
add_subdirectory(src/ui)
add_subdirectory(src/app)
if(UNIX AND NOT APPLE AND APP_BUILD_REMOTE)
    add_subdirectory(src/remote)
endif()

if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

message(STATUS "")
message(STATUS "${PROJECT_NAME} ${APP_VERSION} configuration summary:")
message(STATUS "  Build type        : ${CMAKE_BUILD_TYPE}")
message(STATUS "  System integration: ${APP_SYSTEM_INTEGRATION}")
message(STATUS "  Build remote      : ${APP_BUILD_REMOTE}")
message(STATUS "  Build tests       : ${BUILD_TESTS}")
message(STATUS "  Use KSH           : ${APP_USE_KSYNTAXHIGHLIGHTING}")
message(STATUS "  KSH target        : ${APP_KSH_TARGET}")
message(STATUS "  Use cmark-gfm     : ${APP_USE_CMARK_GFM}")
message(STATUS "  Qt6               : ${Qt6_VERSION}")
message(STATUS "  Embed Python      : ${APP_EMBED_PYTHON}")
if(APP_EMBED_PYTHON)
    message(STATUS "  Python3           : ${Python3_VERSION}")
endif()
message(STATUS "")
