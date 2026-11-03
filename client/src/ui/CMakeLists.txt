# The native window's toolkit (add-native-ui design D2). wb_ui_core is portable (UTF-16 text, Markdown, layout,
# string bundles) so the fuzz and sanitizer jobs build it; wb_ui is the Windows part (Direct2D in software,
# DirectWrite, theme).
set(WORKBENCH_UI_STRINGS ${CMAKE_CURRENT_SOURCE_DIR}/strings)
# The bundles are compiled in (D9); editing one re-runs CMake.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             ${WORKBENCH_UI_STRINGS}/cs.json ${WORKBENCH_UI_STRINGS}/de.json ${WORKBENCH_UI_STRINGS}/en.json)
file(READ ${WORKBENCH_UI_STRINGS}/cs.json WORKBENCH_UI_STRINGS_CS)
file(READ ${WORKBENCH_UI_STRINGS}/de.json WORKBENCH_UI_STRINGS_DE)
file(READ ${WORKBENCH_UI_STRINGS}/en.json WORKBENCH_UI_STRINGS_EN)
configure_file(bundles.cpp.in ${CMAKE_CURRENT_BINARY_DIR}/bundles.cpp @ONLY)

add_library(wb_ui_core STATIC text16.cpp markdown.cpp layout.cpp list.cpp strings.cpp ${CMAKE_CURRENT_BINARY_DIR}/bundles.cpp)
target_include_directories(wb_ui_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(wb_ui_core PUBLIC wb_base PRIVATE md4c nlohmann_json)
workbench_warnings(wb_ui_core)

# The string bundles are checked at build time: a missing key, parameter or plural form fails the build (D9).
add_executable(ui_strings_check strings_check.cpp)
target_link_libraries(ui_strings_check PRIVATE wb_ui_core)
workbench_warnings(ui_strings_check)
if(WIN32)
  target_link_options(ui_strings_check PRIVATE -Wl,--subsystem,console)
endif()
add_custom_command(
  OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/strings.checked
  COMMAND ui_strings_check ${WORKBENCH_UI_STRINGS} ${CMAKE_CURRENT_BINARY_DIR}/strings.checked
  DEPENDS ui_strings_check ${WORKBENCH_UI_STRINGS}/cs.json ${WORKBENCH_UI_STRINGS}/de.json ${WORKBENCH_UI_STRINGS}/en.json
  COMMENT "Checking UI string bundles")
add_custom_target(ui_strings_checked ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/strings.checked)

if(WIN32)
  add_library(wb_ui STATIC device.cpp theme.cpp system_language.cpp canvas.cpp elements.cpp richtext.cpp textfield.cpp uia.cpp window.cpp)
  target_link_libraries(wb_ui PUBLIC wb_ui_core wb_platform wb_base d2d1 d3d11 dxgi dwrite windowscodecs dwmapi
                               uiautomationcore oleacc shcore ole32 oleaut32 shell32 user32)
  add_dependencies(wb_ui ui_strings_checked)
  workbench_warnings(wb_ui)
endif()
