# Model access (client/model-access): gateway client, streaming, errors and retries, tool-call validation, images,
# embeddings and rerank. Windows only (WinHTTP transport, WIC image scaling).
add_library(wb_model STATIC
  wire.cpp
  errors.cpp
  usage.cpp
  image.cpp
  model_client.cpp)
target_link_libraries(wb_model PUBLIC wb_identity wb_platform wb_base windowscodecs ole32 oleaut32 uuid)
skowork_warnings(wb_model)
