add_library(wb_platform STATIC
  win.cpp
  crypto.cpp
  dpapi.cpp
  http.cpp
  websocket.cpp
  registry.cpp
  pipe.cpp
  log.cpp
  crash.cpp)
target_link_libraries(wb_platform PUBLIC wb_base winhttp bcrypt crypt32 advapi32 shell32 ole32 secur32 dbghelp)
skowork_warnings(wb_platform)
