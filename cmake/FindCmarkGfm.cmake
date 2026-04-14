# SPDX-License-Identifier: GPL-3.0-only
#
# Thin pkg-config wrapper around libcmark-gfm and its extensions package.
# The top-level CMakeLists.txt already does the pkg_check_modules call
# inline; this module is a place to hang the fallback logic for
# platforms (macOS Homebrew, Windows vcpkg) that ship cmark-gfm via
# a CMake package config instead of pkg-config. Phase 2+ work.

if(NOT TARGET PkgConfig::CMARK_GFM)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(CMARK_GFM QUIET IMPORTED_TARGET
            libcmark-gfm libcmark-gfm-extensions)
    endif()
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(CmarkGfm
    REQUIRED_VARS CMARK_GFM_FOUND
    VERSION_VAR CMARK_GFM_VERSION)
