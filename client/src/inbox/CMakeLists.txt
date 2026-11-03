# client/work-inbox (add-work-inbox): the inbox state of Threads, views, search, drafts and fork in app.db, and the host
# service behind the single-page app's inbox.
add_library(wb_inbox STATIC inbox_store.cpp inbox_service.cpp)
target_link_libraries(wb_inbox PUBLIC wb_tools wb_runtime wb_host wb_storage wb_platform wb_base)
workbench_warnings(wb_inbox)
