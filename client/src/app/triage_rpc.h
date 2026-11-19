// Event triage's host methods (client/event-triage), UI only: triage.status, .configure, .ack_gap, .delete, .suggestion,
// .start and .dismiss.
#pragma once

#include "host/rpc.h"
#include "triage/triage.h"

#include <functional>

namespace wb {

void register_triage_rpc(RpcRegistry& rpc, std::function<triage::TriageService*()> svc);

}  // namespace wb