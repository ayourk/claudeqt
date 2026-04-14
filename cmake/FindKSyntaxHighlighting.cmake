# SPDX-License-Identifier: GPL-3.0-only
#
# Locate KSyntaxHighlighting, preferring KF6 and falling back to the
# KF5-qt6 backport shipped via ppa:ayourk/claudeqt. The top-level
# CMakeLists.txt already performs this probe inline; this module is
# a placeholder for non-Linux pathways (Homebrew tap, vcpkg registry)
# where the config-file location differs. Phase 2+ work.

if(NOT CLAUDEQT_KSH_TARGET)
    find_package(KF6SyntaxHighlighting QUIET)
    if(KF6SyntaxHighlighting_FOUND)
        set(CLAUDEQT_KSH_TARGET KF6::SyntaxHighlighting)
    else()
        find_package(KF5SyntaxHighlighting QUIET
            HINTS /usr/lib/${CMAKE_LIBRARY_ARCHITECTURE}/kf5-qt6/cmake/KF5SyntaxHighlighting
                  /usr/lib/x86_64-linux-gnu/kf5-qt6/cmake/KF5SyntaxHighlighting
                  /usr/lib/aarch64-linux-gnu/kf5-qt6/cmake/KF5SyntaxHighlighting)
        if(KF5SyntaxHighlighting_FOUND)
            set(CLAUDEQT_KSH_TARGET KF5::SyntaxHighlighting)
        endif()
    endif()
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(KSyntaxHighlighting
    REQUIRED_VARS CLAUDEQT_KSH_TARGET)
