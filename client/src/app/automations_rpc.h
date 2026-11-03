// The Automations section's host methods (client/scheduler Automations Section, Run History): automations.list, .get,
// .save, .delete and .run_now, UI only.
#pragma once

#include "host/rpc.h"
#include "scheduler/scheduler.h"
#include "storage/database.h"

#include <functional>
#include <optional>
#include <string>

namespace wb {

struct AutomationsRpcEnv {
    std::function<scheduler::Scheduler*()> scheduler;  // null while it is not running
    Database* db = nullptr;                            // the Remote task history (remote_tasks)
    // The sequence number of a Run's first message, to open its Thread there (Open Result From History).
    std::function<std::optional<long long>(const std::string& run_id)> first_message_seq;
    std::function<std::string()> local_zone;  // IANA id of the computer's time zone
};

void register_automations_rpc(RpcRegistry& rpc, AutomationsRpcEnv env);

}  // namespace wb
