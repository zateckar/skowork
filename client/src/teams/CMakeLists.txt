# client/teams-chats (add-local-outlook-and-teams). wb_teams_core is portable: the Snappy, LevelDB, IndexedDB and V8 value
# decoders of the new Teams cache, the Teams extraction and the snapshot copy (design D4), so the fuzz job builds it.
# wb_teams is the Windows part: the local Teams copy, sync, events and tools (design D5).
add_library(wb_teams_core STATIC snappy.cpp leveldb.cpp indexeddb.cpp v8value.cpp teams_extract.cpp snapshot.cpp)
target_link_libraries(wb_teams_core PUBLIC wb_base)
skowork_warnings(wb_teams_core)

if(WIN32)
  add_library(wb_teams STATIC teams_service.cpp)
  target_link_libraries(wb_teams PUBLIC wb_teams_core wb_runtime wb_storage wb_platform wb_base)
  skowork_warnings(wb_teams)
endif()
