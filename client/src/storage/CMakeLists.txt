add_library(wb_storage STATIC
  keystore.cpp
  database.cpp
  data_root.cpp
  withdrawal_outbox.cpp
  forget.cpp
  quota.cpp
  app_store.cpp
  agent_skills.cpp
  generated_surface_state.cpp
  generated_surface_records.cpp
  generated_surface_submission.cpp
  schema.cpp)
target_link_libraries(wb_storage PUBLIC wb_platform sqlite3mc PRIVATE wb_generated_surface_discovery)
skowork_warnings(wb_storage)
