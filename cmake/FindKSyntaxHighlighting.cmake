# SPDX-License-Identifier: GPL-3.0-only
#
# Locate KSyntaxHighlighting, preferring KF6 and falling back to a
# KF5-qt6 backport. The top-level CMakeLists.txt already performs
# this probe inline; this module is a placeholder for non-Linux
# pathways (Homebrew tap, vcpkg registry) where the config-file
# location differs.

if(NOT APP_KSH_TARGET)
    find_package(KF6SyntaxHighlighting QUIET)
    if(KF6SyntaxHighlighting_FOUND)
        set(APP_KSH_TARGET KF6::SyntaxHighlighting)
    else()
        find_package(KF5SyntaxHighlighting QUIET
            HINTS /usr/lib/${CMAKE_LIBRARY_ARCHITECTURE}/kf5-qt6/cmake/KF5SyntaxHighlighting
                  /usr/lib/x86_64-linux-gnu/kf5-qt6/cmake/KF5SyntaxHighlighting
                  /usr/lib/aarch64-linux-gnu/kf5-qt6/cmake/KF5SyntaxHighlighting)
        if(KF5SyntaxHighlighting_FOUND)
            set(APP_KSH_TARGET KF5::SyntaxHighlighting)
        endif()
    endif()
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(KSyntaxHighlighting
    REQUIRED_VARS APP_KSH_TARGET)
