# Fuzz targets for parsers of untrusted input (WORKBENCH_FUZZ; rules.tasks: fuzz tests for parsers of untrusted
# input). Each target exports LLVMFuzzerTestOneInput, so it runs under libFuzzer with Clang (the CI fuzz job), and
# otherwise builds the standalone driver in fuzz_main.h: corpus replay, AFL++ file mode, and a built-in mutation mode
# for compilers without libFuzzer (GCC on MinGW).
set(WORKBENCH_FUZZ_CORPUS ${CMAKE_CURRENT_SOURCE_DIR}/corpus)
set(WORKBENCH_MODEL_FIXTURES ${CMAKE_CURRENT_SOURCE_DIR}/../tests/model/fixtures)

function(workbench_fuzz_target name)
  add_executable(${name} ${ARGN})
  target_link_libraries(${name} PRIVATE wb_base)
  workbench_warnings(${name})
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_definitions(${name} PRIVATE WORKBENCH_LIBFUZZER=1)
    target_compile_options(${name} PRIVATE -fsanitize=fuzzer-no-link)
    target_link_options(${name} PRIVATE -fsanitize=fuzzer)
  endif()
endfunction()

workbench_fuzz_target(fuzz_tool_args tool_args_fuzz.cpp)
workbench_fuzz_target(fuzz_sse sse_fuzz.cpp)
workbench_fuzz_target(fuzz_policy_paths policy_paths_fuzz.cpp)
target_link_libraries(fuzz_policy_paths PRIVATE wb_policy_core)
workbench_fuzz_target(fuzz_html html_fuzz.cpp)
target_link_libraries(fuzz_html PRIVATE wb_tools_core)
workbench_fuzz_target(fuzz_ooxml ooxml_fuzz.cpp)
target_link_libraries(fuzz_ooxml PRIVATE wb_extract miniz)
workbench_fuzz_target(fuzz_mcp mcp_fuzz.cpp)
target_link_libraries(fuzz_mcp PRIVATE wb_mcp_core)
workbench_fuzz_target(fuzz_cdp cdp_fuzz.cpp)
target_link_libraries(fuzz_cdp PRIVATE wb_browser_core)
workbench_fuzz_target(fuzz_markdown markdown_fuzz.cpp)
target_link_libraries(fuzz_markdown PRIVATE wb_ui_core)
workbench_fuzz_target(fuzz_telemetry_redaction telemetry_redaction_fuzz.cpp)
target_link_libraries(fuzz_telemetry_redaction PRIVATE wb_telemetry_core)
workbench_fuzz_target(fuzz_skill_md skill_md_fuzz.cpp)
target_link_libraries(fuzz_skill_md PRIVATE wb_skills_core)
workbench_fuzz_target(fuzz_skill_package skill_package_fuzz.cpp)
target_link_libraries(fuzz_skill_package PRIVATE wb_skills_core)
workbench_fuzz_target(fuzz_graph_parse graph_parse_fuzz.cpp)
target_link_libraries(fuzz_graph_parse PRIVATE wb_mail_core)
workbench_fuzz_target(fuzz_kg_chunk kg_chunk_fuzz.cpp)
target_link_libraries(fuzz_kg_chunk PRIVATE wb_knowledge_core)
workbench_fuzz_target(fuzz_kg_analysis kg_analysis_fuzz.cpp)
target_link_libraries(fuzz_kg_analysis PRIVATE wb_knowledge_core)
workbench_fuzz_target(fuzz_meeting_container meeting_container_fuzz.cpp)
target_link_libraries(fuzz_meeting_container PRIVATE wb_meeting_core)
workbench_fuzz_target(fuzz_speech_manifest speech_manifest_fuzz.cpp)
target_link_libraries(fuzz_speech_manifest PRIVATE wb_meeting_core)
if(WIN32)
  # PDF extraction is Windows-only (pdfium.dll); the target runs with the DLL next to it.
  workbench_fuzz_target(fuzz_pdf pdf_fuzz.cpp)
  target_link_libraries(fuzz_pdf PRIVATE wb_extract)
  add_custom_command(TARGET fuzz_pdf POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${TP}/pdfium/x64/pdfium.dll" $<TARGET_FILE_DIR:fuzz_pdf>)
endif()

# A short mutation run of each target as a test, with the standalone driver (ctest -L fuzz).
if(WORKBENCH_TESTS AND NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  file(GLOB tool_args_seeds ${WORKBENCH_FUZZ_CORPUS}/tool_args/*)
  add_test(NAME fuzz_tool_args_smoke
           COMMAND fuzz_tool_args -runs=200000 -seed=1 -dict=${WORKBENCH_FUZZ_CORPUS}/tool_args.dict ${tool_args_seeds})
  add_test(NAME fuzz_sse_smoke
           COMMAND fuzz_sse -runs=50000 -seed=1 -max_len=65536 -dict=${WORKBENCH_FUZZ_CORPUS}/sse.dict
                   ${WORKBENCH_MODEL_FIXTURES}/primary_reasoning_stream.sse
                   ${WORKBENCH_MODEL_FIXTURES}/primary_tool_call_stream.sse
                   ${WORKBENCH_MODEL_FIXTURES}/secondary_tool_call_stream.sse)
  file(GLOB policy_paths_seeds ${WORKBENCH_FUZZ_CORPUS}/policy_paths/*)
  add_test(NAME fuzz_policy_paths_smoke
           COMMAND fuzz_policy_paths -runs=200000 -seed=1 -max_len=4096 -dict=${WORKBENCH_FUZZ_CORPUS}/policy_paths.dict
                   ${policy_paths_seeds})
  file(GLOB html_seeds ${WORKBENCH_FUZZ_CORPUS}/html/*)
  add_test(NAME fuzz_html_smoke
           COMMAND fuzz_html -runs=200000 -seed=1 -max_len=8192 -dict=${WORKBENCH_FUZZ_CORPUS}/html.dict ${html_seeds})
  file(GLOB ooxml_seeds ${WORKBENCH_FUZZ_CORPUS}/ooxml/*)
  add_test(NAME fuzz_ooxml_smoke
           COMMAND fuzz_ooxml -runs=100000 -seed=1 -max_len=65536 -dict=${WORKBENCH_FUZZ_CORPUS}/ooxml.dict ${ooxml_seeds})
  file(GLOB mcp_seeds ${WORKBENCH_FUZZ_CORPUS}/mcp/*)
  add_test(NAME fuzz_mcp_smoke
           COMMAND fuzz_mcp -runs=200000 -seed=1 -max_len=8192 -dict=${WORKBENCH_FUZZ_CORPUS}/mcp.dict ${mcp_seeds})
  file(GLOB cdp_seeds ${WORKBENCH_FUZZ_CORPUS}/cdp/*)
  add_test(NAME fuzz_cdp_smoke
           COMMAND fuzz_cdp -runs=200000 -seed=1 -max_len=8192 -dict=${WORKBENCH_FUZZ_CORPUS}/cdp.dict ${cdp_seeds})
  file(GLOB markdown_seeds ${WORKBENCH_FUZZ_CORPUS}/markdown/*)
  add_test(NAME fuzz_markdown_smoke
           COMMAND fuzz_markdown -runs=200000 -seed=1 -max_len=8192 -dict=${WORKBENCH_FUZZ_CORPUS}/markdown.dict ${markdown_seeds})
  file(GLOB telemetry_redaction_seeds ${WORKBENCH_FUZZ_CORPUS}/telemetry_redaction/*)
  add_test(NAME fuzz_telemetry_redaction_smoke
           COMMAND fuzz_telemetry_redaction -runs=100000 -seed=1 -max_len=4096 -dict=${WORKBENCH_FUZZ_CORPUS}/telemetry_redaction.dict
                   ${telemetry_redaction_seeds})
  file(GLOB skill_md_seeds ${WORKBENCH_FUZZ_CORPUS}/skill_md/*)
  add_test(NAME fuzz_skill_md_smoke
           COMMAND fuzz_skill_md -runs=200000 -seed=1 -max_len=4096 -dict=${WORKBENCH_FUZZ_CORPUS}/skill_md.dict ${skill_md_seeds})
  file(GLOB skill_package_seeds ${WORKBENCH_FUZZ_CORPUS}/skill_package/*)
  add_test(NAME fuzz_skill_package_smoke
           COMMAND fuzz_skill_package -runs=100000 -seed=1 -max_len=8192 ${skill_package_seeds})
  file(GLOB graph_parse_seeds ${WORKBENCH_FUZZ_CORPUS}/graph_parse/*)
  add_test(NAME fuzz_graph_parse_smoke
           COMMAND fuzz_graph_parse -runs=200000 -seed=1 -max_len=8192 -dict=${WORKBENCH_FUZZ_CORPUS}/graph_parse.dict ${graph_parse_seeds})
  file(GLOB kg_chunk_seeds ${WORKBENCH_FUZZ_CORPUS}/kg_chunk/*)
  add_test(NAME fuzz_kg_chunk_smoke
           COMMAND fuzz_kg_chunk -runs=200000 -seed=1 -max_len=8192 -dict=${WORKBENCH_FUZZ_CORPUS}/kg_chunk.dict ${kg_chunk_seeds})
  file(GLOB kg_analysis_seeds ${WORKBENCH_FUZZ_CORPUS}/kg_analysis/*)
  add_test(NAME fuzz_kg_analysis_smoke
           COMMAND fuzz_kg_analysis -runs=200000 -seed=1 -max_len=8192 -dict=${WORKBENCH_FUZZ_CORPUS}/kg_analysis.dict ${kg_analysis_seeds})
  file(GLOB meeting_container_seeds ${WORKBENCH_FUZZ_CORPUS}/meeting_container/*)
  add_test(NAME fuzz_meeting_container_smoke
           COMMAND fuzz_meeting_container -runs=200000 -seed=1 -max_len=8192 -dict=${WORKBENCH_FUZZ_CORPUS}/meeting_container.dict ${meeting_container_seeds})
  add_test(NAME fuzz_speech_manifest_smoke COMMAND fuzz_speech_manifest -runs=200000 -seed=1 -max_len=8192)
  set_tests_properties(fuzz_tool_args_smoke fuzz_sse_smoke fuzz_policy_paths_smoke fuzz_html_smoke fuzz_ooxml_smoke fuzz_mcp_smoke
                       fuzz_cdp_smoke fuzz_markdown_smoke fuzz_telemetry_redaction_smoke fuzz_skill_md_smoke fuzz_skill_package_smoke
                       fuzz_graph_parse_smoke fuzz_kg_chunk_smoke fuzz_kg_analysis_smoke fuzz_meeting_container_smoke
                       fuzz_speech_manifest_smoke PROPERTIES LABELS fuzz TIMEOUT 600)
  if(WIN32)
    file(GLOB pdf_seeds ${WORKBENCH_FUZZ_CORPUS}/pdf/*)
    add_test(NAME fuzz_pdf_smoke COMMAND fuzz_pdf -runs=20000 -seed=1 -max_len=16384 ${pdf_seeds})
    set_tests_properties(fuzz_pdf_smoke PROPERTIES LABELS fuzz TIMEOUT 600)
  endif()
endif()
