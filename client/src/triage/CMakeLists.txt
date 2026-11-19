# The classifier is portable (triage_tests); the service needs the Windows mail, runtime and storage libraries.
add_library(wb_triage_core STATIC classifier.cpp)
target_link_libraries(wb_triage_core PUBLIC wb_base)
skowork_warnings(wb_triage_core)
if(WIN32)
  add_library(wb_triage STATIC triage.cpp)
  target_link_libraries(wb_triage PUBLIC wb_triage_core wb_mail wb_runtime wb_model wb_storage wb_platform)
  skowork_warnings(wb_triage)
endif()
