# client/mail-calendar (add-mail-calendar). wb_mail_core is portable (configuration, label and exclusion rules, the
# Graph JSON parsers) so the fuzz job builds it; wb_mail is the Windows part (OAuth, the HTTP layer, the store, sync, tools).
add_library(wb_mail_core STATIC mail_config.cpp filter.cpp graph_parse.cpp)
target_link_libraries(wb_mail_core PUBLIC wb_base)
skowork_warnings(wb_mail_core)

if(WIN32)
  add_library(wb_mail STATIC graph_auth.cpp graph_client.cpp mail_store.cpp mail_sync.cpp mail_service.cpp mail_tools.cpp)
  target_link_libraries(wb_mail PUBLIC wb_mail_core wb_identity wb_tools wb_runtime wb_storage wb_platform wb_base)
  skowork_warnings(wb_mail)
endif()
