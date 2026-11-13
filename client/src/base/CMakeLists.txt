# Portable code: no Win32 headers (design D9). Built on Windows and on the Linux sanitizer job.
add_library(wb_base STATIC
  encoding.cpp
  url.cpp
  jwt.cpp
  redact.cpp
  version.cpp
  backoff.cpp
  time.cpp
  strings.cpp
  sse.cpp
  tool_args.cpp
  json_schema.cpp)
target_include_directories(wb_base PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(wb_base PUBLIC nlohmann_json)
skowork_warnings(wb_base)
