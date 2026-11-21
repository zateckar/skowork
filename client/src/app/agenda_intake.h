// The app's Agenda intake adapters and the preparation tool rule (client/agenda; add-agenda design D1, D5): meeting
// transcripts and pushed Chat messages become SourceItems, and preparation Runs may call read-risk tools only.
#pragma once

#include "agenda/core.h"
#include "base/json.h"
#include "meeting/meeting_store.h"
#include "runtime/hooks.h"
#include "teams/teams_service.h"

#include <functional>
#include <optional>
#include <string>

namespace wb {

// A long transcript keeps its first third and its end, where follow-ups are usually agreed, within kMaxItemText.
std::string agenda_bounded_transcript(const std::string& text);

// A meeting addresses nobody: admission needs the candidate or the transcript to name the user. Empty for a meeting
// without transcript text.
std::optional<agenda::SourceItem> agenda_meeting_item(const meeting::MeetingRecord& m, const json& turns);

// `payload` is the chat-message push ({"chat", "message"}), `chat` the Chat it belongs to (or null when unknown) and
// `me` the user's messaging id. Direct Chats address the user; in a team Chat the user must be named.
std::optional<agenda::SourceItem> agenda_chat_item(const json& payload, const json& chat, const std::string& me, long long now_ms);

// A Teams message (client/agenda Agenda Item Sources; add-local-outlook-and-teams design D6): the preceding messages of
// its conversation as context, addressed when the Teams event says so, its id "<conversation>|<message>".
agenda::SourceItem agenda_teams_item(const teams::StoredTeamsMessage& m, const teams::StoredConversation& c, const std::string& conversation_name,
                                     const std::vector<teams::StoredTeamsMessage>& before, bool addressed);

// Preparation Runs and their Child Runs get read-risk tools only (Preparation Has No Effects).
runtime::ToolDecision agenda_preparation_decision(const runtime::ToolCallInfo& call,
                                                  const std::function<bool(const std::string&)>& is_preparation_run);

}  // namespace wb
