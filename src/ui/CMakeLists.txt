# SPDX-License-Identifier: GPL-3.0-only
#
# ${PROJECT_NAME}_ui — GUI widgets, Theme singleton, ThemedQtDialog
# base class, MainWindow. Built as a STATIC library so the main app
# and any test binaries that exercise dialog behavior can both link
# it without pulling in duplicate QObject moc output. Target name
# derives from ${PROJECT_NAME} so a fork's project() rename
# propagates automatically.

add_library(${PROJECT_NAME}_ui STATIC
    actionregistry.h
    actionregistry.cpp
    theme.h
    theme.cpp
    themedqtdialog.h
    themedqtdialog.cpp
    rightclicksplitter.h
    rightclicksplitter.cpp
    codeeditor.h
    codeeditor.cpp
    editorpanewidget.h
    editorpanewidget.cpp
    messagewindowwidget.h
    messagewindowwidget.cpp
    chatinputwidget.h
    chatinputwidget.cpp
    projectsessiontreemodel.h
    projectsessiontreemodel.cpp
    settingsdialog.h
    settingsdialog.cpp
    mainwindow.h
    mainwindow.cpp
    ${CMAKE_SOURCE_DIR}/resources/resources.qrc)

set_target_properties(${PROJECT_NAME}_ui PROPERTIES
    AUTOMOC ON)

target_include_directories(${PROJECT_NAME}_ui PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR})

# About tab bakes compile-time version strings in as
# -D macros. KSH version comes from the KF5/KF6 probe, cmark-gfm
# from pkg-config, CMake + build type from the configure state.
# Runtime-queryable values (Qt, SQLite, OS) stay as API calls.
target_compile_definitions(${PROJECT_NAME}_ui PRIVATE
    APP_VERSION="${APP_VERSION}"
    APP_NAME="${PROJECT_NAME}"
    APP_HOMEPAGE="${PROJECT_HOMEPAGE_URL}"
    APP_ISSUES_URL="${PROJECT_ISSUES_URL}"
    APP_DESCRIPTION="${PROJECT_DESCRIPTION}"
    APP_CMAKE_VERSION_STRING="${CMAKE_VERSION}"
    APP_BUILD_TYPE_STRING="${CMAKE_BUILD_TYPE}")
if(PROJECT_PPA AND EXISTS "/etc/debian_version")
    target_compile_definitions(${PROJECT_NAME}_ui PRIVATE
        APP_PPA="${PROJECT_PPA}")
endif()
if(APP_USE_KSYNTAXHIGHLIGHTING)
    target_compile_definitions(${PROJECT_NAME}_ui PRIVATE
        APP_USE_KSYNTAXHIGHLIGHTING)
    if(KF6SyntaxHighlighting_FOUND)
        target_compile_definitions(${PROJECT_NAME}_ui PRIVATE
            APP_KSH_VERSION_STRING="${KF6SyntaxHighlighting_VERSION}")
    elseif(KF5SyntaxHighlighting_VERSION)
        target_compile_definitions(${PROJECT_NAME}_ui PRIVATE
            APP_KSH_VERSION_STRING="${KF5SyntaxHighlighting_VERSION}")
    endif()
endif()
if(CMARK_GFM_VERSION)
    target_compile_definitions(${PROJECT_NAME}_ui PRIVATE
        APP_CMARK_GFM_VERSION_STRING="${CMARK_GFM_VERSION}")
endif()

target_link_libraries(${PROJECT_NAME}_ui PUBLIC
    ${PROJECT_NAME}_persistence
    ${APP_KSH_TARGET}
    Qt6::Core
    Qt6::Gui
    Qt6::Svg
    Qt6::Widgets
    Qt6::Sql
    SQLite::SQLite3)
