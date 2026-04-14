# SPDX-License-Identifier: GPL-3.0-only
#
# Top-level CMakeLists for claudeqt. See docs/phase-1.md §6 for the
# rationale behind the version range, the dependency probes, and the
# Qt6+KF5-qt6 fallback order.
#
# Floor 3.22 = Jammy stock (22.04 LTS), so future Jammy ports don't
# need a newer CMake. Upper 4.3 enables modern policy defaults under
# CMake 4.x (e.g. a future Resolute chroot) without requiring it.
cmake_minimum_required(VERSION 3.22...4.3)

project(claudeqt
    VERSION 0.0.1
    DESCRIPTION "Native Qt6 GUI for Claude Code CLI"
    HOMEPAGE_URL "https://github.com/ayourk/claudeqt"
    LANGUAGES C CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

option(CLAUDEQT_SYSTEM_INTEGRATION
    "Install systemd units, desktop entry, and icons to FHS system paths" OFF)
option(CLAUDEQT_BUILD_REMOTE
    "Build the claudeqt-remote daemon (Linux only)" ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

include(GNUInstallDirs)

# --- Qt6 probe (cctv-viewer pattern; 6.4.2 floor matches Noble stock) ---
find_package(Qt6 6.4.2 REQUIRED COMPONENTS
    Core Gui Widgets Svg Network Sql LinguistTools)

# --- KSyntaxHighlighting: try KF6 first, fall back to KF5-qt6 ---
#
# Phase 1 ships the KF5-qt6 backport (see packaging/ppa/kf5syntaxhighlighting-qt6/)
# which installs under the R6-isolated tree /usr/lib/<arch>/kf5-qt6/ rather than
# the stock KDE path. The HINTS below points at that isolated CMake config tree,
# which differs from phase-1.md §6's original hint path
# (/usr/lib/<arch>/cmake/KF5SyntaxHighlighting-Qt6) — the divergence is driven by
# the R6 collision-isolation decision made during debianization (phase-1.md §11
# Decision 5 + debian-rebase rules KDE_INSTALL_LIBDIR override). KF6 builds on
# Resolute pick up via the first branch and never touch the hint.
find_package(KF6SyntaxHighlighting QUIET)
if(KF6SyntaxHighlighting_FOUND)
    set(CLAUDEQT_KSH_TARGET KF6::SyntaxHighlighting)
    message(STATUS "Using KF6 SyntaxHighlighting ${KF6SyntaxHighlighting_VERSION}")
else()
    find_package(KF5SyntaxHighlighting 5.115 REQUIRED
        HINTS /usr/lib/${CMAKE_LIBRARY_ARCHITECTURE}/kf5-qt6/cmake/KF5SyntaxHighlighting
              /usr/lib/x86_64-linux-gnu/kf5-qt6/cmake/KF5SyntaxHighlighting
              /usr/lib/aarch64-linux-gnu/kf5-qt6/cmake/KF5SyntaxHighlighting)
    set(CLAUDEQT_KSH_TARGET KF5::SyntaxHighlighting)
    message(STATUS "Using KF5 SyntaxHighlighting Qt6 backport ${KF5SyntaxHighlighting_VERSION}")
endif()

# --- cmark-gfm via pkg-config ---
#
# Noble's libcmark-gfm-dev ships libcmark-gfm.pc, but libcmark-gfm-extensions-dev
# does NOT provide a .pc file — it only installs the .so and headers. So we
# probe the core module via pkg-config and locate the extensions library by hand.
find_package(PkgConfig REQUIRED)
pkg_check_modules(CMARK_GFM REQUIRED IMPORTED_TARGET libcmark-gfm)
find_library(CMARK_GFM_EXTENSIONS_LIBRARY
    NAMES cmark-gfm-extensions
    REQUIRED)
find_path(CMARK_GFM_EXTENSIONS_INCLUDE_DIR
    NAMES cmark-gfm-core-extensions.h
    REQUIRED)

# --- SQLite (Qt6::Sql links it, but we need the C headers for PRAGMAs) ---
find_package(SQLite3 3.45 REQUIRED)

# --- libsystemd for sd_notify() in claudeqt-remote (Linux only) ---
if(UNIX AND NOT APPLE AND CLAUDEQT_BUILD_REMOTE)
    pkg_check_modules(SYSTEMD REQUIRED IMPORTED_TARGET libsystemd)
endif()

# --- Python embedding: wired in Phase 2, floor stated here for early visibility ---
find_package(Python3 3.12 REQUIRED COMPONENTS Development Interpreter)

add_subdirectory(src/app)
if(UNIX AND NOT APPLE AND CLAUDEQT_BUILD_REMOTE)
    add_subdirectory(src/remote)
endif()

if(BUILD_TESTING)
    enable_testing()
    add_subdirectory(tests)
endif()

message(STATUS "")
message(STATUS "claudeqt ${PROJECT_VERSION} configuration summary:")
message(STATUS "  Build type        : ${CMAKE_BUILD_TYPE}")
message(STATUS "  System integration: ${CLAUDEQT_SYSTEM_INTEGRATION}")
message(STATUS "  Build remote      : ${CLAUDEQT_BUILD_REMOTE}")
message(STATUS "  KSH target        : ${CLAUDEQT_KSH_TARGET}")
message(STATUS "  Qt6               : ${Qt6_VERSION}")
message(STATUS "  Python3           : ${Python3_VERSION}")
message(STATUS "")
