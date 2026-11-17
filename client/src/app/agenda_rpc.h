// The Agenda view's and settings section's host methods (client/agenda, client/work-inbox Agenda View), UI only:
// agenda.status, .set, .list, .done, .dismiss, .reopen, .edit, .prepare and .delete_all.
#pragma once

#include "agenda/service.h"
#include "host/rpc.h"

#include <functional>

namespace wb {

void register_agenda_rpc(RpcRegistry& rpc, std::function<agenda::Service*()> svc);

}  // namespace wb
