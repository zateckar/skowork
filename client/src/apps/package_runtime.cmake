# The app runtime package (client/local-apps App Runtime, add-local-apps 2.6): node.exe and its LICENSE from
# client/runtime/node (dev/scripts/fetch-node-runtime.ps1) and app-host.js, laid out as runtime/node next to
# Skowork.exe. Fails when the package exceeds its budget of 100 MB.
#   cmake -DSRC_RUNTIME=<client/runtime/node> -DAPP_HOST=<app-host.js> -DDEST=<bin/runtime/node> -P package_runtime.cmake
file(MAKE_DIRECTORY "${DEST}")
file(COPY_FILE "${APP_HOST}" "${DEST}/app-host.js" ONLY_IF_DIFFERENT)
if(NOT EXISTS "${SRC_RUNTIME}/node.exe")
  message(STATUS "app runtime: ${SRC_RUNTIME}/node.exe is missing (run dev/scripts/fetch-node-runtime.ps1); Local apps stay unavailable")
  return()
endif()
foreach(f node.exe LICENSE)
  if(EXISTS "${SRC_RUNTIME}/${f}")
    file(COPY_FILE "${SRC_RUNTIME}/${f}" "${DEST}/${f}" ONLY_IF_DIFFERENT)
  endif()
endforeach()
set(total 0)
file(GLOB files "${DEST}/*")
foreach(f ${files})
  file(SIZE "${f}" s)
  math(EXPR total "${total} + ${s}")
endforeach()
set(budget 104857600)  # 100 MB
math(EXPR mb "${total} / 1048576")
if(total GREATER budget)
  message(FATAL_ERROR "app runtime package is ${total} bytes (${mb} MB), over its budget of 100 MB")
endif()
message(STATUS "app runtime package: ${total} bytes (${mb} MB of 100 MB)")
