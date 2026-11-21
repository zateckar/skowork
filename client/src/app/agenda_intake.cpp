#include "app/agenda_intake.h"
#include "app/agenda_reply.h"

#include "base/strings.h"
#include "base/time.h"

namespace wb {

agenda::SourceItem agenda_teams_item(const teams::StoredTeamsMessage& m, const teams::StoredConversation& c, const std::string& conversation_name,
                                     const std::vector<teams::StoredTeamsMessage>& before, bool addressed) {
    const auto who = [](const teams::StoredTeamsMessage& x) { return x.sender_name.empty() ? std::string("Someone") : x.sender_name; };
    std::string context;
    for (const auto& p : before)
        if (p.id != m.id && p.at_ms <= m.at_ms) context += who(p) + ": " + p.text + "\n";
    agenda::SourceItem s;
    s.kind = "teams";
    s.id = m.conversation_id + "|" + m.id;
    s.conversation = m.conversation_id;
    s.title = conversation_name;
    s.text = utf8_truncate((context.empty() ? std::string{} : "Earlier in this conversation:\n" + context + "\nNew message:\n") + who(m) + ": " + m.text,
                           agenda::kMaxItemText);
    s.author = m.sender_name.empty() ? m.sender_email : m.sender_name;
    s.author_is_user = m.mine;
    s.user_addressed = addressed;
    s.recipients = c.type == "chat" ? 1 : c.type == "channel" ? agenda::kBroadcastRecipients : 8;
    s.at_ms = m.at_ms;
    s.link = "teams:" + m.conversation_id;
    return s;
}

json agenda_reply_prefill(const json& item, const std::string& draft,
                          const std::function<std::optional<mail::StoredMessage>(const std::string& id)>& mail_message,
                          const std::function<std::optional<TeamsReplySource>(const std::string& id)>& teams_message) {
    json out = {{"id", jstr(item, "id")}, {"body", draft},  {"to", json::array()}, {"cc", json::array()},
                {"subject", jstr(item, "title")}, {"reply_to", ""}, {"source", ""}};
    for (const auto& s : item.value("sources", json::array())) {
        const std::string kind = jstr(s, "kind"), sid = jstr(s, "id");
        if (kind == "mail" && mail_message) {
            const auto m = mail_message(sid);
            if (!m || m->withheld || m->from.address.empty()) continue;
            out["to"] = json::array({m->from.address});
            const std::string subj = m->subject;
            out["subject"] = to_lower(subj).rfind("re:", 0) == 0 ? subj : "RE: " + subj;
            out["reply_to"] = m->id;
            out["source"] = "mail";
            return out;
        }
        if (kind == "teams" && teams_message) {
            const auto t = teams_message(sid);
            if (!t) continue;
            if (!t->message.sender_email.empty()) out["to"] = json::array({t->message.sender_email});
            out["subject"] = "RE: " + t->conversation_name;
            out["source"] = "teams";
            return out;
        }
    }
    return out;
}

std::string agenda_bounded_transcript(const std::string& text) {
    if (text.size() <= agenda::kMaxItemText) return text;
    const std::string head = utf8_truncate(text, agenda::kMaxItemText / 3);
    size_t from = text.size() - (agenda::kMaxItemText - head.size() - 8);
    while (from < text.size() && (static_cast<unsigned char>(text[from]) & 0xC0) == 0x80) ++from;
    return head + "\n[...]\n" + text.substr(from);
}

std::optional<agenda::SourceItem> agenda_meeting_item(const meeting::MeetingRecord& m, const json& turns) {
    agenda::SourceItem s;
    s.kind = "meeting";
    s.id = m.id;
    s.conversation = m.series_id.empty() ? m.event_id : m.series_id;
    s.title = m.title;
    for (const auto& x : turns) s.text += jstr(x, "speaker") + ": " + jstr(x, "text") + "\n";
    s.text = agenda_bounded_transcript(s.text);
    s.recipients = static_cast<int>(m.attendees.size());
    s.at_ms = m.ended_at ? m.ended_at : m.started_at;
    s.link = m.thread_id.empty() ? std::string{} : "#/threads/" + m.thread_id;
    if (trim(s.text).empty()) return std::nullopt;
    return s;
}

std::optional<agenda::SourceItem> agenda_chat_item(const json& payload, const json& chat, const std::string& me, long long now_ms) {
    const json* msg = jobj(payload, "message");
    if (!msg) return std::nullopt;
    agenda::SourceItem s;
    s.kind = "chat";
    s.id = jstr(*msg, "id");
    s.conversation = jstr(payload, "chat", jstr(*msg, "chat"));
    s.text = jstr(*msg, "text");
    s.author = jstr(*msg, "sender_name", jstr(*msg, "sender"));
    s.author_is_user = iequals(jstr(*msg, "sender"), me);
    s.at_ms = parse_iso8601_ms(jstr(*msg, "at")).value_or(now_ms);
    const bool direct = chat.is_object() && jstr(chat, "kind") == "direct";
    s.title = chat.is_object() ? jstr(chat, "name", direct ? s.author : std::string("Chat")) : std::string("Chat");
    if (s.title.empty()) s.title = direct ? s.author : "Chat";
    s.recipients = chat.is_object() ? static_cast<int>(chat.value("members", json::array()).size()) : 0;
    s.user_addressed = direct;
    s.link = "#/chats/" + s.conversation;
    if (s.id.empty() || s.conversation.empty() || trim(s.text).empty()) return std::nullopt;
    return s;
}

runtime::ToolDecision agenda_preparation_decision(const runtime::ToolCallInfo& call,
                                                  const std::function<bool(const std::string&)>& is_preparation_run) {
    if (call.risk == runtime::Risk::read || !is_preparation_run) return runtime::ToolDecision::allow();
    if (is_preparation_run(call.run.run_id) || (!call.run.parent_run_id.empty() && is_preparation_run(call.run.parent_run_id)))
        return runtime::ToolDecision::deny("Agenda preparation only reads; it does not send, write or change anything.");
    return runtime::ToolDecision::allow();
}

}  // namespace wb
