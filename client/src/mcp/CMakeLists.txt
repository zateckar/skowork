# client/mcp-client (add-core-tools-and-mcp design D3, D8). wb_mcp_core is portable (JSON-RPC, MCP messages, framing,
# catalog filters, result conversion) so the fuzz job builds it; wb_mcp is the Windows part (transports, manager).
add_library(wb_mcp_core STATIC protocol.cpp)
target_link_libraries(wb_mcp_core PUBLIC wb_base)
workbench_warnings(wb_mcp_core)

if(WIN32)
  add_library(wb_mcp STATIC session.cpp manager.cpp)
  target_link_libraries(wb_mcp PUBLIC wb_mcp_core wb_policy wb_sandbox wb_runtime wb_platform wb_base PRIVATE miniz)
  workbench_warnings(wb_mcp)
endif()
