# client/evaluation (add-evaluation-export): the instruction registry, the user decision log, quality counters,
# evaluation export and replay.
add_library(wb_eval STATIC registry.cpp decision_log.cpp)
target_include_directories(wb_eval PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(wb_eval PUBLIC wb_triage_core wb_agenda_core wb_knowledge_core wb_storage wb_platform wb_base)
skowork_warnings(wb_eval)
