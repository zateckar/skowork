// Task tools for the agent (client/scheduler Task Tools For The Agent): tasks.list (read), tasks.create, tasks.update,
// tasks.delete and tasks.run_now (write-local). Triggers they create are labelled "created by agent"; the policy
// decides each call like any other tool call.
#pragma once

#include "runtime/tools.h"
#include "scheduler/scheduler.h"

#include <functional>
#include <vector>

namespace wb::scheduler {

// `get` returns the scheduler, or null while it is not running (the tools then fail with that reason).
std::vector<runtime::ToolDef> task_tools(std::function<Scheduler*()> get);

}  // namespace wb::scheduler
