# SPDX-License-Identifier: GPL-3.0-only
#
# Resolve CLAUDEQT_VERSION from debian/changelog's topmost entry,
# falling back to the project() version if parsing fails or no
# debian/changelog is present (non-Linux source builds).
#
# debian/changelog format: "package (version) distro; urgency=..."
# We extract "version" from the first line and use it as-is.

function(claudeqt_read_version out_var)
    set(_changelog "${CMAKE_CURRENT_SOURCE_DIR}/debian/changelog")
    if(NOT EXISTS "${_changelog}")
        set(${out_var} "${PROJECT_VERSION}" PARENT_SCOPE)
        return()
    endif()

    file(READ "${_changelog}" _head LIMIT 256)
    if(_head MATCHES "^[^ ]+ *\\(([^)]+)\\)")
        set(${out_var} "${CMAKE_MATCH_1}" PARENT_SCOPE)
    else()
        set(${out_var} "${PROJECT_VERSION}" PARENT_SCOPE)
    endif()
endfunction()
