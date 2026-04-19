# SPDX-License-Identifier: GPL-3.0-only
#
# QtTest-based unit tests. Wired from the top-level CMakeLists.txt
# via `add_subdirectory(tests)` behind the BUILD_TESTS option.
# Each test is a standalone executable added to ctest via
# add_test().

find_package(Qt6 REQUIRED COMPONENTS Test Widgets)

# Core-only tests (no QWidgets). QTEST_MAIN picks QCoreApplication.
function(app_add_core_test name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE
        ${PROJECT_NAME}_persistence
        Qt6::Core
        Qt6::Sql
        Qt6::Test)
    set_target_properties(${name} PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endfunction()

# GUI tests that need Qt6::Widgets (ThemedQtDialog, MainWindow, …).
# QTEST_MAIN picks QApplication because Widgets is linked.
function(app_add_gui_test name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE
        ${PROJECT_NAME}_ui
        ${PROJECT_NAME}_persistence
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
        Qt6::Sql
        Qt6::Test)
    set_target_properties(${name} PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endfunction()

app_add_core_test(test_persistence)
app_add_core_test(test_instancehub)

# Helper binary for the multi-process IPC test. Not a test itself —
# spawned by test_instancehub_ipc via QProcess.
add_executable(hub_child_helper hub_child_helper.cpp)
target_link_libraries(hub_child_helper PRIVATE
    ${PROJECT_NAME}_persistence
    Qt6::Core
    Qt6::Test)
set_target_properties(hub_child_helper PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")

app_add_core_test(test_instancehub_ipc)

app_add_gui_test(test_theme)
app_add_gui_test(test_actionregistry)
app_add_gui_test(test_themedqtdialog)
app_add_gui_test(test_mainwindow)
app_add_gui_test(test_codeeditor)
app_add_gui_test(test_chatshell)
app_add_gui_test(test_chatsplitter)
app_add_gui_test(test_treemodel)
app_add_gui_test(test_treecrud)
app_add_gui_test(test_settingsdialog)
