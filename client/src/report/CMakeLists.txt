# client/daily-report (add-local-outlook-and-teams design D7). Portable: settings, schedule, digest and the service, whose
# host side (collecting, the Thread and Run) is supplied by the app.
add_library(wb_report STATIC report_core.cpp daily_report.cpp)
target_link_libraries(wb_report PUBLIC wb_base)
skowork_warnings(wb_report)
