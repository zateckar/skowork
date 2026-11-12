# The native window's screens (add-native-ui group 3). wb_shell_core is portable: the view models ported from the
# web UI (feed fold, Thread list order and grouping, routes, shortcuts, status derivations, formatting), unit-tested
# everywhere. wb_shell is the Windows part: the main window and its views on the wb_ui toolkit.
add_library(wb_shell_core STATIC model.cpp feed.cpp inbox_model.cpp routes.cpp derive.cpp shortcuts.cpp format.cpp)
target_include_directories(wb_shell_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(wb_shell_core PUBLIC wb_ui_core wb_base nlohmann_json)
workbench_warnings(wb_shell_core)

if(WIN32)
  add_library(wb_shell STATIC main_window.cpp shell.cpp views_frame.cpp views_list.cpp views_thread.cpp views_feed.cpp views_composer.cpp views_search.cpp views_pages.cpp views_connectors.cpp views_skill_editor.cpp views_automations.cpp views_mail.cpp views_knowledge.cpp views_knowledge_view.cpp views_meetings.cpp views_apps.cpp views_chats.cpp views_dictation.cpp views_speech.cpp)
  target_link_libraries(wb_shell PUBLIC wb_shell_core wb_ui)
  workbench_warnings(wb_shell)
endif()
