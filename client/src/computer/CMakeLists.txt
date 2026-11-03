# client/computer-use (add-computer-use). wb_computer_core is portable (action classification, scoping, hotkeys, the
# screenshot coordinate transform); wb_computer is the Windows part (UI Automation, capture, input, the overlay, tools).
add_library(wb_computer_core STATIC classify.cpp)
target_link_libraries(wb_computer_core PUBLIC wb_base)
workbench_warnings(wb_computer_core)

if(WIN32)
  add_library(wb_computer STATIC uia.cpp desktop.cpp input.cpp capture.cpp control.cpp service.cpp)
  target_link_libraries(wb_computer PUBLIC wb_computer_core wb_runtime wb_policy wb_model wb_platform wb_base
                                            d3d11 dxgi dwmapi runtimeobject windowscodecs ole32 oleaut32 uuid)
  workbench_warnings(wb_computer)
endif()
