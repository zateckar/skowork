// The Chats view's host methods (client/user-messaging Chats View, Own Presence), UI only: messaging.status, .chats,
// .messages (opens a Chat: what is new, and the read position), .send, .directory, .create, .rename, .add_members,
// .leave and .busy.
#pragma once

#include "host/rpc.h"
#include "messaging/messaging.h"

#include <functional>

namespace wb {

void register_messaging_rpc(RpcRegistry& rpc, std::function<messaging::MessagingService*()> svc);

}  // namespace wb
