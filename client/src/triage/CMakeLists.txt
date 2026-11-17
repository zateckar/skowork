add_library(wb_triage STATIC classifier.cpp)
target_link_libraries(wb_triage PUBLIC wb_base)
skowork_warnings(wb_triage)
