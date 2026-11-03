# Compiler and linker flags (add-client-foundation design D9).

option(WORKBENCH_WERROR "Treat warnings as errors (off only in the Clang fuzz job, whose warning set differs)" ON)
set(WORKBENCH_WARNINGS -Wall -Wextra -Wpedantic -Wshadow -Wconversion $<$<BOOL:${WORKBENCH_WERROR}>:-Werror>)
set(CMAKE_C_FLAGS_RELEASE "-O2 -DNDEBUG")
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG")
set(CMAKE_C_FLAGS_RELWITHDEBINFO "-O2 -g -DNDEBUG")
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "-O2 -g -DNDEBUG")

add_compile_options(-ffunction-sections -fdata-sections -fstack-protector-strong -D_GLIBCXX_ASSERTIONS
                    $<$<CONFIG:Release>:-D_FORTIFY_SOURCE=3>)

if(WORKBENCH_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
  add_link_options(-fsanitize=address,undefined)
elseif(WIN32)
  # Trap-mode UBSan needs no runtime library on MinGW (design D9).
  add_compile_options($<$<CONFIG:Debug>:-fsanitize=undefined> $<$<CONFIG:Debug>:-fsanitize-trap=all>)
endif()

if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
  add_compile_options(-fcf-protection=full)
endif()

if(WIN32)
  # Windows 11 target; process-loopback and AUTOMATIC_PROXY need at least Windows 10 2004.
  add_compile_definitions(UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00 WINVER=0x0A00
                          NTDDI_VERSION=0x0A000008)
  add_link_options(-static -Wl,--dynamicbase,--nxcompat,--high-entropy-va -Wl,--gc-sections
                   $<$<CONFIG:Release>:-s>)
endif()

if(WORKBENCH_DEV_ENDPOINTS)
  add_compile_definitions(WORKBENCH_DEV_ENDPOINTS=1)
endif()

function(workbench_warnings target)
  target_compile_options(${target} PRIVATE ${WORKBENCH_WARNINGS})
endfunction()
