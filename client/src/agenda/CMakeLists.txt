# client/agenda (add-agenda). wb_agenda_core is portable (detection request and parser, admission, due dates, merge
# matching) so the fuzz job builds it; wb_agenda is the Windows part (store, intake, service, preparation, RPC).
add_library(wb_agenda_core STATIC core.cpp)
target_link_libraries(wb_agenda_core PUBLIC wb_base)
skowork_warnings(wb_agenda_core)

if(WIN32)
  add_library(wb_agenda STATIC store.cpp service.cpp)
  target_link_libraries(wb_agenda PUBLIC wb_agenda_core wb_eval_core wb_storage wb_model wb_base)
  skowork_warnings(wb_agenda)
endif()
