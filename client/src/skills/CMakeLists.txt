# client/skills (add-skills). wb_skills_core is portable (SKILL.md, frontmatter, tiers, tool scope) so the Linux
# sanitizer and fuzz jobs build it.
add_library(wb_skills_core STATIC yaml.cpp skill.cpp catalog.cpp package.cpp sync.cpp authoring.cpp agent_save.cpp)
target_link_libraries(wb_skills_core PUBLIC wb_base PRIVATE miniz)
workbench_warnings(wb_skills_core)
