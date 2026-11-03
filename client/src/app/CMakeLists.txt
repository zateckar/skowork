# Workbench.exe: wiring and resources.

# VERSIONINFO takes four numbers: the numeric part of the version (a pre-release suffix is dropped), then 0.
string(REGEX MATCH "^[0-9]+\\.[0-9]+\\.[0-9]+" _vnum "${WORKBENCH_VERSION}")
if(NOT _vnum)
  message(FATAL_ERROR "WORKBENCH_VERSION must start with MAJOR.MINOR.PATCH, got '${WORKBENCH_VERSION}'")
endif()
string(REPLACE "." "," WORKBENCH_VERSION_COMMAS "${_vnum}")
configure_file(build_info.h.in ${CMAKE_CURRENT_BINARY_DIR}/gen/app/build_info.h @ONLY)
configure_file(app.rc.in ${CMAKE_CURRENT_BINARY_DIR}/app.rc @ONLY)
set_source_files_properties(${CMAKE_CURRENT_BINARY_DIR}/app.rc PROPERTIES OBJECT_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/app.manifest;${CMAKE_CURRENT_SOURCE_DIR}/workbench.ico")

add_library(wb_app STATIC app.cpp agent_service.cpp skill_authoring.cpp automations_rpc.cpp mail_rpc.cpp knowledge_rpc.cpp meeting_rpc.cpp apps_rpc.cpp messaging_rpc.cpp)
target_include_directories(wb_app PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/.. ${CMAKE_CURRENT_BINARY_DIR}/gen)
target_link_libraries(wb_app PUBLIC wb_telemetry wb_shell wb_inbox wb_scheduler wb_meeting wb_knowledge wb_mail wb_browser wb_computer wb_apps wb_messaging wb_mcp wb_tools wb_policy wb_sandbox wb_runtime wb_model wb_host wb_channel wb_identity wb_storage wb_platform wb_base secur32 wtsapi32)
workbench_warnings(wb_app)

add_executable(Workbench WIN32 main.cpp ${CMAKE_CURRENT_BINARY_DIR}/app.rc)
target_link_libraries(Workbench PRIVATE wb_app)
# Our manifest replaces MinGW's default one.
target_link_options(Workbench PRIVATE -municode -Wl,--no-insert-timestamp)
workbench_warnings(Workbench)
set_target_properties(Workbench PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
add_dependencies(Workbench workbench-worker workbench-app-runtime)
