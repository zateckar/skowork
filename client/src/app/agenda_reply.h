// The send form of a prepared draft (client/agenda Sending A Prepared Draft; add-local-outlook-and-teams design D9).
// Kept apart from agenda_intake.h so that header does not bring the mail namespace into its users.
#pragma once

#include "base/json.h"
#include "mail/types.h"
#include "teams/teams_service.h"

#include <functional>
#include <optional>
#include <string>

namespace wb {

struct TeamsReplySource {
    teams::StoredTeamsMessage message;
    std::string conversation_name;
};

// {id, body, to, cc, subject, reply_to, source} from the Agenda item (agenda.list shape), the draft text, and lookups of
// its mail and Teams sources. The first mail or Teams source that is still available decides the recipient and subject.
// A mail reply goes to the sender with the other recipients in Cc (reply-all); `me`, the user's address, is never a
// recipient, and a reply to the user's own message is not addressed to the user.
json agenda_reply_prefill(const json& item, const std::string& draft,
                          const std::function<std::optional<mail::StoredMessage>(const std::string& id)>& mail_message,
                          const std::function<std::optional<TeamsReplySource>(const std::string& id)>& teams_message,
                          const std::string& me = {});

}  // namespace wb
