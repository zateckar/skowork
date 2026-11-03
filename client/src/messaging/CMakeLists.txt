# client/user-messaging (add-user-messaging): the messaging service with its cache in app.db, Presence and the messages.*
# tools.
add_library(wb_messaging STATIC messaging.cpp messaging_tools.cpp)
target_link_libraries(wb_messaging PUBLIC wb_runtime wb_storage wb_platform wb_base)
workbench_warnings(wb_messaging)
