# SPDX-License-Identifier: GPL-3.0-only

add_executable(claudeqt
    main.cpp)

target_compile_definitions(claudeqt PRIVATE
    CLAUDEQT_VERSION="${PROJECT_VERSION}")

target_link_libraries(claudeqt PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Svg
    Qt6::Network
    Qt6::Sql
    ${CLAUDEQT_KSH_TARGET}
    PkgConfig::CMARK_GFM
    ${CMARK_GFM_EXTENSIONS_LIBRARY}
    SQLite::SQLite3)

target_include_directories(claudeqt PRIVATE
    ${CMARK_GFM_EXTENSIONS_INCLUDE_DIR})

set_target_properties(claudeqt PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

install(TARGETS claudeqt
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

if(CLAUDEQT_SYSTEM_INTEGRATION AND UNIX AND NOT APPLE)
    # desktop entry, icons, and /usr/share/claudeqt/ payloads are
    # installed from resources/ in Phase 2+. This block reserves the
    # option hook so debian/rules can toggle it on today without a
    # downstream diff.
endif()
