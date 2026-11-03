add_library(wb_channel STATIC
  machine_config.cpp
  control_plane.cpp
  control_channel.cpp)
target_link_libraries(wb_channel PUBLIC wb_identity wb_storage wb_platform)
workbench_warnings(wb_channel)
