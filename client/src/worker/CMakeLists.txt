add_executable(workbench-worker WIN32 main.cpp handlers.cpp)
target_include_directories(workbench-worker PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(workbench-worker PRIVATE wb_extract wb_knowledge_core wb_meeting_audio wb_platform wb_base)
target_link_options(workbench-worker PRIVATE -municode)
workbench_warnings(workbench-worker)
set_target_properties(workbench-worker PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
# pdfium.dll ships next to the worker, which alone loads it (add-core-tools-and-mcp design D4).
add_custom_command(TARGET workbench-worker POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${TP}/pdfium/x64/pdfium.dll" "${CMAKE_BINARY_DIR}/bin/"
  # The speech-to-text runtime belongs to the models package (add-meeting-capture design D1): models\stt beside the worker.
  COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/bin/models/stt"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${TP}/sherpa-onnx/x64/sherpa-onnx-c-api.dll" "${TP}/sherpa-onnx/x64/onnxruntime.dll" "${CMAKE_BINARY_DIR}/bin/models/stt/")
