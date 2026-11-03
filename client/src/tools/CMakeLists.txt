# client/core-tools (add-core-tools-and-mcp). wb_tools_core is portable (text handling, HTML extraction, the destructive
# command classifier, image headers) so the fuzz and sanitizer jobs build it; wb_tools is the Windows part (the tools themselves).
add_library(wb_tools_core STATIC text.cpp shell_classify.cpp html.cpp image_info.cpp)
target_link_libraries(wb_tools_core PUBLIC wb_base)
workbench_warnings(wb_tools_core)

if(WIN32)
  add_library(wb_tools STATIC files.cpp shell.cpp web.cpp image.cpp quarantine.cpp extractor.cpp)
  target_link_libraries(wb_tools PUBLIC wb_tools_core wb_policy wb_sandbox wb_runtime wb_platform wb_base)
  workbench_warnings(wb_tools)
endif()
