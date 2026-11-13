add_library(wb_identity STATIC
  loopback_listener.cpp
  oidc.cpp
  token_manager.cpp)
target_link_libraries(wb_identity PUBLIC wb_storage wb_platform ws2_32)
skowork_warnings(wb_identity)
