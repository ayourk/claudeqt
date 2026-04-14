# SPDX-License-Identifier: GPL-3.0-only

add_executable(claudeqt-remote
    main.cpp)

target_compile_definitions(claudeqt-remote PRIVATE
    CLAUDEQT_VERSION="${PROJECT_VERSION}")

target_link_libraries(claudeqt-remote PRIVATE
    PkgConfig::SYSTEMD)

set_target_properties(claudeqt-remote PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

install(TARGETS claudeqt-remote
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
