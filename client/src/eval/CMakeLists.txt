# client/evaluation (add-evaluation-export). Core: the instruction registry, the user decision log and the quality
# counters, used by the feature services. wb_eval: export and replay, which read those services' stores.
add_library(wb_eval_core STATIC registry.cpp decision_log.cpp quality.cpp)
target_include_directories(wb_eval_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(wb_eval_core PUBLIC wb_triage_core wb_agenda_core wb_knowledge_core wb_telemetry_core wb_storage wb_platform wb_base)
skowork_warnings(wb_eval_core)

add_library(wb_eval STATIC jsonl.cpp exporter.cpp replay.cpp eval_jobs.cpp)
target_link_libraries(wb_eval PUBLIC wb_eval_core wb_triage wb_mail wb_teams wb_meeting wb_model)
skowork_warnings(wb_eval)
