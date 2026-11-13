# Document Text Extraction (add-core-tools-and-mcp design D4). Portable OOXML part (fuzzed on every platform); the
# PDFium loader on Windows. Linked only into skowork-worker.exe (the sandboxed extract role) and the tests.
add_library(wb_extract STATIC ooxml.cpp)
if(WIN32)
  target_sources(wb_extract PRIVATE pdf_win.cpp)
endif()
target_include_directories(wb_extract PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(wb_extract PUBLIC wb_base PRIVATE miniz pugixml)
skowork_warnings(wb_extract)
