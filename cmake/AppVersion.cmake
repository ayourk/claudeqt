# SPDX-License-Identifier: GPL-3.0-only
#
# Resolve APP_VERSION from debian/changelog's topmost entry, falling
# back to the project() version if parsing fails or no debian/changelog
# is present (non-Linux source builds).
#
# When built from a git repo outside CI, the short commit hash is
# appended (e.g. "0.0.1+g1a2b3c4") so local builds are traceable.
# CI builds (detected via the CI environment variable) produce a
# clean version string for release artifacts.
#
# debian/changelog format: "package (version) distro; urgency=..."
# Fork-neutral: reads from the project's own debian/ dir regardless
# of the app name. Forks that preserve the debian/ scaffold inherit
# this machinery for free.

function(app_read_version out_var)
    set(_changelog "${CMAKE_CURRENT_SOURCE_DIR}/debian/changelog")
    if(NOT EXISTS "${_changelog}")
        set(_version "${PROJECT_VERSION}")
    else()
        file(READ "${_changelog}" _head LIMIT 256)
        if(_head MATCHES "^[^ ]+ *\\(([^)]+)\\)")
            set(_raw "${CMAKE_MATCH_1}")
            string(REGEX REPLACE "[-~].*" "" _version "${_raw}")
        else()
            set(_version "${PROJECT_VERSION}")
        endif()
    endif()

    if(NOT DEFINED ENV{CI})
        find_package(Git QUIET)
        if(Git_FOUND)
            execute_process(
                COMMAND "${GIT_EXECUTABLE}" rev-parse --short HEAD
                WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
                OUTPUT_VARIABLE _hash
                OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET
                RESULT_VARIABLE _git_rc)
            if(_git_rc EQUAL 0 AND _hash)
                string(APPEND _version "+g${_hash}")
            endif()
        endif()
    endif()

    set(${out_var} "${_version}" PARENT_SCOPE)
endfunction()
