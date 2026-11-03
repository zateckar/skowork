// The Mail and calendar settings section's host methods (client/mail-calendar): mail.status, .connect, .disconnect,
// .sync_now, .exclusions.add and .exclusions.remove, UI only.
#pragma once

#include "host/rpc.h"
#include "mail/mail_service.h"

#include <functional>

namespace wb {

// `service`: the running mail service, or null while it is not running.
void register_mail_rpc(RpcRegistry& rpc, std::function<mail::MailService*()> service);

}  // namespace wb
