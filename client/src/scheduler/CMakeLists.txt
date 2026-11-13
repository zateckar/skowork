# Scheduler (client/scheduler): Triggers, Occurrences, cron in IANA time zones (icu.dll), Remote task Occurrences.
add_library(wb_scheduler STATIC
  cron.cpp
  icu_zone.cpp
  scheduler.cpp
  file_watcher.cpp
  task_tools.cpp
  remote_tasks.cpp
  mail_sources.cpp
  skill_triggers.cpp)
target_link_libraries(wb_scheduler PUBLIC wb_runtime wb_storage wb_platform wb_base)
skowork_warnings(wb_scheduler)
