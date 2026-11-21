# client/mail-calendar (add-mail-calendar, add-local-outlook-and-teams). wb_mail_core is portable (configuration, label
# and exclusion rules, the Graph JSON parsers, the Outlook-to-Graph adapter) so the fuzz job builds it; wb_mail is the
# Windows part (OAuth, the HTTP layer, the store, sync, tools, the Outlook object-model session).
add_library(wb_mail_core STATIC mail_config.cpp filter.cpp graph_parse.cpp outlook_graph.cpp)
target_link_libraries(wb_mail_core PUBLIC wb_base)
skowork_warnings(wb_mail_core)

if(WIN32)
  add_library(wb_mail STATIC graph_auth.cpp graph_client.cpp mail_store.cpp mail_sync.cpp mail_service.cpp mail_tools.cpp
                             com_outlook_session.cpp)
  target_link_libraries(wb_mail PUBLIC wb_mail_core wb_identity wb_tools wb_runtime wb_storage wb_platform wb_base ole32 oleaut32 uuid)
  skowork_warnings(wb_mail)
endif()
