# client/local-apps (add-local-apps): Local apps on the bundled Node.js runtime, the loopback listener with the MCP
# bridge, and the apps.* tools. app-host.js ships in the app runtime package next to node.exe.
add_library(wb_apps STATIC apps.cpp app_server.cpp app_tools.cpp)
target_link_libraries(wb_apps PUBLIC wb_policy wb_sandbox wb_runtime wb_platform wb_base ws2_32)
workbench_warnings(wb_apps)

# The app runtime package in bin/runtime/node, with its size check (package_runtime.cmake).
add_custom_target(workbench-app-runtime ALL
  COMMAND ${CMAKE_COMMAND} -DSRC_RUNTIME=${PROJECT_SOURCE_DIR}/runtime/node
          -DAPP_HOST=${CMAKE_CURRENT_SOURCE_DIR}/app-host.js -DDEST=${CMAKE_BINARY_DIR}/bin/runtime/node
          -P ${CMAKE_CURRENT_SOURCE_DIR}/package_runtime.cmake
  COMMENT "app runtime package"
  VERBATIM)
