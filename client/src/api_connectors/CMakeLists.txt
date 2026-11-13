# client/api-connectors (add-api-connectors design D5, D11): API connectors as tools, their calls through the control plane,
# and the personal access token calls. Windows only, like the MCP manager it sits beside.
if(WIN32)
  add_library(wb_api_connectors STATIC connectors.cpp)
  target_link_libraries(wb_api_connectors PUBLIC wb_policy wb_runtime wb_platform wb_base)
  skowork_warnings(wb_api_connectors)
endif()
