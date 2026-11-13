# client/browser-use (add-browser-use). wb_browser_core is portable (CDP framing, the page snapshot, action
# classification) so the fuzz and sanitizer jobs build it; wb_browser is the Windows part (Chrome, the pipe, the tools).
add_library(wb_browser_core STATIC cdp_frame.cpp snapshot.cpp classify.cpp)
target_link_libraries(wb_browser_core PUBLIC wb_base)
skowork_warnings(wb_browser_core)

if(WIN32)
  add_library(wb_browser STATIC cdp.cpp chrome.cpp service.cpp)
  target_link_libraries(wb_browser PUBLIC wb_browser_core wb_runtime wb_tools wb_sandbox wb_policy wb_model wb_platform
                                           wb_base version)
  skowork_warnings(wb_browser)
endif()
