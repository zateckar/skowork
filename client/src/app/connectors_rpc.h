// The Connectors settings section's host methods (client/api-connectors): connectors.list, connectors.token.set and
// connectors.token.remove, UI only. The token is a parameter of connectors.token.set and is never logged or returned.
#pragma once

#include "api_connectors/connectors.h"
#include "host/rpc.h"

#include <functional>

namespace wb {

// `request`: the control-plane request of the signed-in client, or an empty function while it is not registered.
void register_connectors_rpc(RpcRegistry& rpc, std::function<connectors::Request()> request);

}  // namespace wb
