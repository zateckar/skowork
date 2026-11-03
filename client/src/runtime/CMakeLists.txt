# Agent runtime (client/agent-runtime): Run state machine and event log, tool-calling loop, hooks and the mandatory
# policy handler, find_tools, Questions and approvals, queue and steering, cancellation. Windows only (Job Objects).
add_library(wb_runtime STATIC
  hooks.cpp
  tools.cpp
  run_store.cpp
  context.cpp
  run_context.cpp
  runtime.cpp)
target_link_libraries(wb_runtime PUBLIC wb_skills_core wb_model wb_storage wb_platform wb_base shell32 ole32)
workbench_warnings(wb_runtime)
