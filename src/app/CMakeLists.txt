# SPDX-License-Identifier: GPL-3.0-only

add_executable(${PROJECT_NAME}
    main.cpp)

set(_app_svg "${CMAKE_SOURCE_DIR}/resources/icons/app.svg")

if(APPLE)
    # Derive the bundle identifier from the homepage URL so forks
    # that change project() get a unique identifier automatically.
    # "https://github.com/user/repo" → "com.github.user.repo"
    string(REGEX REPLACE "^https?://" "" _bundle_host "${PROJECT_HOMEPAGE_URL}")
    string(REPLACE "/" "." _bundle_id "${_bundle_host}")
    string(REGEX REPLACE "\\.$" "" _bundle_id "${_bundle_id}")

    # Generate .icns from SVG at build time.
    # rsvg-convert (librsvg) renders the SVG to PNGs;
    # iconutil (ships with Xcode) packs the .iconset into .icns.
    find_program(RSVG_CONVERT rsvg-convert)
    set(_iconset_dir "${CMAKE_CURRENT_BINARY_DIR}/app.iconset")
    set(_icns_file "${CMAKE_CURRENT_BINARY_DIR}/app.icns")
    if(RSVG_CONVERT)
        set(_icon_sizes 16 32 64 128 256 512)
        set(_png_outputs "")
        foreach(_sz IN LISTS _icon_sizes)
            math(EXPR _sz2 "${_sz} * 2")
            list(APPEND _png_outputs
                "${_iconset_dir}/icon_${_sz}x${_sz}.png"
                "${_iconset_dir}/icon_${_sz}x${_sz}@2x.png")
            add_custom_command(
                OUTPUT "${_iconset_dir}/icon_${_sz}x${_sz}.png"
                COMMAND ${CMAKE_COMMAND} -E make_directory "${_iconset_dir}"
                COMMAND ${RSVG_CONVERT} -w ${_sz} -h ${_sz}
                    "${_app_svg}" -o "${_iconset_dir}/icon_${_sz}x${_sz}.png"
                DEPENDS "${_app_svg}")
            add_custom_command(
                OUTPUT "${_iconset_dir}/icon_${_sz}x${_sz}@2x.png"
                COMMAND ${CMAKE_COMMAND} -E make_directory "${_iconset_dir}"
                COMMAND ${RSVG_CONVERT} -w ${_sz2} -h ${_sz2}
                    "${_app_svg}" -o "${_iconset_dir}/icon_${_sz}x${_sz}@2x.png"
                DEPENDS "${_app_svg}")
        endforeach()
        add_custom_command(
            OUTPUT "${_icns_file}"
            COMMAND iconutil -c icns "${_iconset_dir}" -o "${_icns_file}"
            DEPENDS ${_png_outputs})
        add_custom_target(app_icon DEPENDS "${_icns_file}")
        add_dependencies(${PROJECT_NAME} app_icon)
        set_source_files_properties("${_icns_file}" PROPERTIES
            MACOSX_PACKAGE_LOCATION Resources)
        target_sources(${PROJECT_NAME} PRIVATE "${_icns_file}")
        set(_bundle_icon_file "app.icns")
    else()
        set(_bundle_icon_file "")
        message(STATUS "rsvg-convert not found — macOS bundle will have no icon")
    endif()

    set_target_properties(${PROJECT_NAME} PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "${PROJECT_NAME}"
        MACOSX_BUNDLE_GUI_IDENTIFIER "${_bundle_id}"
        MACOSX_BUNDLE_BUNDLE_VERSION "${APP_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${APP_VERSION}"
        MACOSX_BUNDLE_ICON_FILE "${_bundle_icon_file}")
elseif(WIN32)
    # Generate .ico from SVG at build time and compile a .rc resource.
    file(GLOB _magick_hints "C:/Program Files/ImageMagick-*")
    find_program(MAGICK magick HINTS ${_magick_hints})
    set(_ico_file "${CMAKE_CURRENT_BINARY_DIR}/app.ico")
    set(_rc_file "${CMAKE_CURRENT_BINARY_DIR}/app.rc")
    if(MAGICK)
        add_custom_command(
            OUTPUT "${_ico_file}"
            COMMAND ${MAGICK} -background none
                "${_app_svg}" -define icon:auto-resize=256,64,48,32,16
                "${_ico_file}"
            DEPENDS "${_app_svg}")
        add_custom_target(app_icon DEPENDS "${_ico_file}")
        add_dependencies(${PROJECT_NAME} app_icon)
        string(REPLACE "\\" "/" _ico_file_fwd "${_ico_file}")
        file(WRITE "${_rc_file}" "IDI_ICON1 ICON \"${_ico_file_fwd}\"\n")
        target_sources(${PROJECT_NAME} PRIVATE "${_rc_file}")
    else()
        message(STATUS "ImageMagick not found — Windows exe will have no icon")
    endif()
endif()

target_compile_definitions(${PROJECT_NAME} PRIVATE
    APP_VERSION="${APP_VERSION}"
    APP_NAME="${PROJECT_NAME}"
    APP_HOMEPAGE="${PROJECT_HOMEPAGE_URL}"
    APP_DESCRIPTION="${PROJECT_DESCRIPTION}")

target_link_libraries(${PROJECT_NAME} PRIVATE
    ${PROJECT_NAME}_persistence
    ${PROJECT_NAME}_ui
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Svg
    Qt6::Network
    Qt6::Sql
    ${APP_KSH_TARGET}
    SQLite::SQLite3)

if(APPLE)
    # Static Qt cocoa plugin uses OpenGL functions directly.
    find_library(OPENGL_FRAMEWORK OpenGL)
    if(OPENGL_FRAMEWORK)
        target_link_libraries(${PROJECT_NAME} PRIVATE ${OPENGL_FRAMEWORK})
    endif()
endif()

if(APP_USE_CMARK_GFM)
    target_link_libraries(${PROJECT_NAME} PRIVATE
        PkgConfig::CMARK_GFM
        ${CMARK_GFM_EXTENSIONS_LIBRARY})
    target_include_directories(${PROJECT_NAME} PRIVATE
        ${CMARK_GFM_EXTENSIONS_INCLUDE_DIR})
endif()

set_target_properties(${PROJECT_NAME} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

install(TARGETS ${PROJECT_NAME}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    BUNDLE DESTINATION .)

if(APP_SYSTEM_INTEGRATION AND UNIX AND NOT APPLE)
    # Desktop integration payload lands in the claudeqt-common
    # arch:all package (see debian/claudeqt-common.install). The
    # app SVG ships to the hicolor scalable-apps tree, and the
    # desktop entry to /usr/share/applications. Both live in
    # resources/ in-tree; AUTORCC baking for the in-binary icon
    # lookup (:/icons/app.svg) is independent — that stays.
    install(FILES
        "${CMAKE_SOURCE_DIR}/resources/icons/app.svg"
        DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/scalable/apps"
        RENAME "${PROJECT_NAME}.svg")
    install(FILES
        "${CMAKE_SOURCE_DIR}/resources/${PROJECT_NAME}.desktop"
        DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/applications")
    # Classic Debian menu entry for window managers that still
    # consume /usr/share/menu/<package> via update-menus (fluxbox,
    # wmaker, icewm, etc.). dh_installmenu is deprecated in recent
    # debhelper compat levels, so we install the file directly
    # rather than route through it. The menu package's trigger on
    # /usr/share/menu/ rebuilds the aggregated menu on install.
    install(FILES
        "${CMAKE_SOURCE_DIR}/resources/${PROJECT_NAME}.menu"
        DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/menu"
        RENAME "${PROJECT_NAME}")
endif()
