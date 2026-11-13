add_library(wb_host STATIC
  dispatch.cpp
  rpc.cpp
  tray.cpp
  registration.cpp
  power.cpp
  workers.cpp
  toast.cpp)
target_link_libraries(wb_host PUBLIC wb_platform wb_base shlwapi user32 gdi32 shell32 ole32 runtimeobject)
skowork_warnings(wb_host)
