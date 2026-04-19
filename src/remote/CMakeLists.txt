# SPDX-License-Identifier: GPL-3.0-only

add_executable(claudeqt-remote
    main.cpp)

target_compile_definitions(claudeqt-remote PRIVATE
    APP_VERSION="${APP_VERSION}")

target_link_libraries(claudeqt-remote PRIVATE
    PkgConfig::SYSTEMD)

set_target_properties(claudeqt-remote PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

install(TARGETS claudeqt-remote
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# /etc/claudeqt/remote.conf — referenced by the service unit's
# ExecStart=. CMAKE_INSTALL_FULL_SYSCONFDIR resolves to /etc
# under a /usr install prefix (Debian) and /usr/local/etc under
# the default prefix, always absolute. debhelper auto-marks
# files under /etc as dpkg conffiles. The current watchdog-only
# binary ignores argv and this file is inert, but shipping it
# makes the ExecStart= contract honest and gives the eventual
# real daemon a settled conffile home.
install(FILES remote.conf
    DESTINATION "${CMAKE_INSTALL_FULL_SYSCONFDIR}/claudeqt")
