// The agent's tool `skills.save` (client/skills Agent Skill Save; add-agent-created-skills design D1, D4, D10): saves a
// private Skill the user asked for. Risk write-local, so policy.tools.risk decides it; attended top-level Runs only.
#pragma once

#include "runtime/tools.h"
#include "skills/agent_save.h"

#include <functional>

namespace wb {

// `env_of` builds the save environment at each call, so the catalogue and the records are the current ones.
runtime::ToolDef skill_save_tool(std::function<skills::AgentSkillEnv()> env_of);

}  // namespace wb
