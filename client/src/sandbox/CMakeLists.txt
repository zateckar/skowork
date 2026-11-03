# client/tool-sandbox (add-policy-and-sandbox): contained child processes, the egress proxy, enforcement level.
add_library(wb_sandbox STATIC sandbox.cpp proxy.cpp enforcement.cpp)
target_link_libraries(wb_sandbox PUBLIC wb_policy_core wb_platform wb_base ws2_32 advapi32)
workbench_warnings(wb_sandbox)
