configure_file(worker.rc.in ${CMAKE_CURRENT_BINARY_DIR}/worker.rc @ONLY)
set_source_files_properties(${CMAKE_CURRENT_BINARY_DIR}/worker.rc PROPERTIES OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/worker.manifest")
add_executable(skowork-worker WIN32 main.cpp handlers.cpp ${CMAKE_CURRENT_BINARY_DIR}/worker.rc)
target_include_directories(skowork-worker PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(skowork-worker PRIVATE wb_extract wb_knowledge_core wb_meeting_audio wb_platform wb_base)
target_link_options(skowork-worker PRIVATE -municode)
if(SKOWORK_MANIFEST_LINK_OPTION)
  target_link_options(skowork-worker PRIVATE "${SKOWORK_MANIFEST_LINK_OPTION}")
endif()
skowork_warnings(skowork-worker)
set_target_properties(skowork-worker PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
# pdfium.dll ships next to the worker, which alone loads it (add-core-tools-and-mcp design D4).
add_custom_command(TARGET skowork-worker POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${TP}/pdfium/x64/pdfium.dll" "${CMAKE_BINARY_DIR}/bin/")
# The speech-to-text runtime is not copied here: it comes only from the speech package under the data root (task 5.6).
