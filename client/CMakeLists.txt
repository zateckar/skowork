# Workbench desktop client (add-client-foundation design D2a, D9, D10).
#
# Windows: GCC on MinGW-w64 builds everything. Other platforms (the Linux sanitizer/fuzz CI job) build only
# the portable libraries under src/base and their tests.
cmake_minimum_required(VERSION 3.25)
project(workbench_client VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(WORKBENCH_TESTS "Build unit tests" ON)
option(WORKBENCH_DEV_ENDPOINTS "Accept http:// loopback endpoints (debug and test builds only, design D10)" OFF)
option(WORKBENCH_SANITIZE "Build with -fsanitize=address,undefined (Linux job only)" OFF)
option(WORKBENCH_FUZZ "Build fuzz targets (AFL++ or libFuzzer-compatible entry points)" OFF)
set(WORKBENCH_CHANNEL "dev" CACHE STRING "Build channel reported in About and at registration")
set(WORKBENCH_COMMIT "unknown" CACHE STRING "Commit identifier reported in About and at registration")
set(WORKBENCH_VERSION "${PROJECT_VERSION}" CACHE STRING "Product version")

set(TP ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
include(cmake/Flags.cmake)

# --- third-party ----------------------------------------------------------------------------------------
add_library(nlohmann_json INTERFACE)
target_include_directories(nlohmann_json SYSTEM INTERFACE ${TP}/nlohmann_json)
target_compile_definitions(nlohmann_json INTERFACE JSON_USE_IMPLICIT_CONVERSIONS=0)

# Document Text Extraction (add-core-tools-and-mcp design D4): the zip container and the OOXML parts.
add_library(miniz STATIC ${TP}/miniz/miniz.c)
target_include_directories(miniz SYSTEM PUBLIC ${TP}/miniz)
target_compile_definitions(miniz PUBLIC MINIZ_NO_STDIO MINIZ_NO_TIME MINIZ_NO_ZLIB_COMPATIBLE_NAMES)
target_compile_options(miniz PRIVATE -w)
add_library(pugixml STATIC ${TP}/pugixml/pugixml.cpp)
target_include_directories(pugixml SYSTEM PUBLIC ${TP}/pugixml)
target_compile_options(pugixml PRIVATE -w)
# Markdown of model output (add-native-ui design D4).
add_library(md4c STATIC ${TP}/md4c/md4c.c)
target_include_directories(md4c SYSTEM PUBLIC ${TP}/md4c)
target_compile_options(md4c PRIVATE -w)

# --- portable libraries ---------------------------------------------------------------------------------
add_subdirectory(src/base)
add_subdirectory(src/policy)
add_subdirectory(src/extract)
add_subdirectory(src/tools)
add_subdirectory(src/telemetry)
add_subdirectory(src/mcp)
add_subdirectory(src/browser)
add_subdirectory(src/computer)  # the Windows part is guarded inside
add_subdirectory(src/skills)  # likewise
add_subdirectory(src/ui)  # the Windows part is guarded inside
add_subdirectory(src/shell)  # likewise
add_subdirectory(src/mail)  # likewise
add_subdirectory(src/knowledge)  # likewise
add_subdirectory(src/meeting)  # likewise

if(WIN32)
  if(NOT MINGW OR NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    message(FATAL_ERROR "The Windows client is built with GCC on MinGW-w64 (design D2a).")
  endif()
  include(cmake/Sqlite.cmake)
  add_subdirectory(src/platform)
  add_subdirectory(src/storage)
  add_subdirectory(src/identity)
  add_subdirectory(src/channel)
  add_subdirectory(src/model)
  add_subdirectory(src/runtime)
  add_subdirectory(src/scheduler)
  add_subdirectory(src/sandbox)
  add_subdirectory(src/apps)
  add_subdirectory(src/messaging)
  add_subdirectory(src/host)
  add_subdirectory(src/inbox)
  add_subdirectory(src/app)
  add_subdirectory(src/worker)
endif()

if(WORKBENCH_TESTS)
  enable_testing()
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
  set(gtest_force_shared_crt OFF CACHE BOOL "" FORCE)
  set(gtest_disable_pthreads OFF CACHE BOOL "" FORCE)
  add_subdirectory(${TP}/googletest ${CMAKE_BINARY_DIR}/googletest EXCLUDE_FROM_ALL)
  add_subdirectory(tests)
endif()

if(WORKBENCH_FUZZ)
  add_subdirectory(fuzz)
endif()
