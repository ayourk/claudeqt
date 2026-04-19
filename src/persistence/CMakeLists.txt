# SPDX-License-Identifier: GPL-3.0-only
#
# ${PROJECT_NAME}_persistence — SQLite DAO singleton. Built as a
# STATIC library so the main app and the test binaries can both
# link it without pulling in two copies of the schema. Target name
# derives from ${PROJECT_NAME} so a fork's project() rename
# propagates automatically.

add_library(${PROJECT_NAME}_persistence STATIC
    persistence.h
    persistence.cpp
    migrations.h
    migrations.cpp
    instancehub.h
    instancehub.cpp)

set_target_properties(${PROJECT_NAME}_persistence PROPERTIES
    AUTOMOC ON)

target_include_directories(${PROJECT_NAME}_persistence PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(${PROJECT_NAME}_persistence PUBLIC
    Qt6::Core
    Qt6::Network
    Qt6::Sql)
