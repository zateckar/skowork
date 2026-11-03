# client/policy-enforcement (add-policy-and-sandbox).
# wb_policy_core is portable (bundle model, glob, decision function, path and egress rules) so the Linux sanitizer and
# fuzz jobs build it; wb_policy is the Windows part (canonicalisation, bundle manager, approval log, enforcer).
add_library(wb_policy_core STATIC core.cpp decide.cpp)
target_link_libraries(wb_policy_core PUBLIC wb_base)
workbench_warnings(wb_policy_core)

# The Windows part needs targets that are only defined on Windows; it is declared from there (see below).
if(WIN32)
  add_library(wb_policy STATIC paths.cpp manager.cpp store.cpp enforcer.cpp egress.cpp)
  # wb_runtime etc. are added after this directory; target_link_libraries resolves names at generate time.
  target_link_libraries(wb_policy PUBLIC wb_policy_core wb_runtime wb_storage wb_platform wb_base)
  workbench_warnings(wb_policy)
endif()
