add_library(wb_storage STATIC
  keystore.cpp
  database.cpp
  data_root.cpp
  forget.cpp
  quota.cpp
  app_store.cpp
  schema.cpp)
target_link_libraries(wb_storage PUBLIC wb_platform sqlite3mc)
workbench_warnings(wb_storage)
