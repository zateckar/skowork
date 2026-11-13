add_library(wb_storage STATIC
  keystore.cpp
  database.cpp
  data_root.cpp
  withdrawal_outbox.cpp
  forget.cpp
  quota.cpp
  app_store.cpp
  agent_skills.cpp
  schema.cpp)
target_link_libraries(wb_storage PUBLIC wb_platform sqlite3mc)
skowork_warnings(wb_storage)
