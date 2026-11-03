# client/telemetry (add-telemetry-and-admin). wb_telemetry_core is portable (the catalogue allowlist, records,
# redaction, the OTLP encoder) so the fuzz and sanitizer jobs build it.
add_library(wb_telemetry_core STATIC record.cpp otlp.cpp pattern.cpp)
target_link_libraries(wb_telemetry_core PUBLIC wb_base)
workbench_warnings(wb_telemetry_core)

if(WIN32)
  add_library(wb_telemetry STATIC service.cpp collector.cpp)
  target_link_libraries(wb_telemetry PUBLIC wb_telemetry_core wb_policy wb_runtime wb_model wb_storage wb_platform wb_base)
  workbench_warnings(wb_telemetry)
endif()
