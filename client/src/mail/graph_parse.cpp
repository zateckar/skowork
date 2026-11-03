#include "mail/graph_parse.h"

#include "base/strings.h"
#include "base/time.h"

namespace wb::mail {
namespace {

std::string text_of(const json& item, const char* key) {
    const auto it = item.find(key);
    return it != item.end() && it->is_string() ? it->get<std::string>() : std::string{};
}

bool bool_of(const json& item, const char* key) {
    const auto it = item.find(key);
    return it != item.end() && it->is_boolean() && it->get<bool>();
}

std::string body_of(const json& item) {
    const auto b = item.find("body");
    if (b == item.end() || !b->is_object()) return {};
    return text_of(*b, "content");
}

std::vector<Person> people_of(const json& item, const char* key) {
    std::vector<Person> out;
    const auto it = item.find(key);
    if (it == item.end() || !it->is_array()) return out;
    for (const auto& r : *it)
        if (auto p = person_of(r); !p.address.empty()) out.push_back(std::move(p));
    return out;
}

std::vector<std::string> categories_of(const json& item) {
    std::vector<std::string> out;
    const auto it = item.find("categories");
    if (it == item.end() || !it->is_array()) return out;
    for (const auto& c : *it)
        if (c.is_string()) out.push_back(c.get<std::string>());
    return out;
}

json person_json(const Person& p) { return {{"name", p.name}, {"address", p.address}}; }

json people_json(const std::vector<Person>& v) {
    json a = json::array();
    for (const auto& p : v) a.push_back(person_json(p));
    return a;
}

}  // namespace

std::optional<long long> graph_time(const json& v) {
    if (v.is_string()) return parse_iso8601_ms(v.get<std::string>());
    if (!v.is_object()) return std::nullopt;
    // Graph answers in UTC unless a Prefer: outlook.timezone header asks otherwise, and the client never sends one.
    const std::string dt = text_of(v, "dateTime");
    if (dt.empty()) return std::nullopt;
    return parse_iso8601_ms(dt);
}

Person person_of(const json& recipient) {
    if (!recipient.is_object()) return {};
    const auto e = recipient.find("emailAddress");
    if (e == recipient.end() || !e->is_object()) return {};
    return {text_of(*e, "name"), text_of(*e, "address")};
}

std::optional<StoredMessage> parse_message(const json& item, const Rules& rules, const std::vector<std::string>& folder_names,
                                           Disposition* disposition) {
    if (!item.is_object()) return std::nullopt;
    StoredMessage m;
    m.id = text_of(item, "id");
    if (m.id.empty()) return std::nullopt;
    const Disposition d = classify(rules, item, folder_names);
    if (disposition) *disposition = d;
    m.folder_id = text_of(item, "parentFolderId");
    m.change_key = text_of(item, "changeKey");
    const auto received = item.find("receivedDateTime");
    m.received_at = received != item.end() ? graph_time(*received).value_or(0) : 0;
    m.label = item_label(item);
    if (d != Disposition::include) {
        m.withheld = true;
        return m;
    }
    m.conversation_id = text_of(item, "conversationId");
    m.subject = text_of(item, "subject");
    if (const auto f = item.find("from"); f != item.end()) m.from = person_of(*f);
    m.to = people_of(item, "toRecipients");
    m.cc = people_of(item, "ccRecipients");
    m.body = body_of(item);
    m.preview = text_of(item, "bodyPreview");
    m.has_attachments = bool_of(item, "hasAttachments");
    m.is_read = bool_of(item, "isRead");
    m.is_draft = bool_of(item, "isDraft");
    m.categories = categories_of(item);
    m.web_link = text_of(item, "webLink");
    return m;
}

std::optional<StoredEvent> parse_event(const json& item, const Rules& rules, Disposition* disposition) {
    if (!item.is_object()) return std::nullopt;
    StoredEvent e;
    e.id = text_of(item, "id");
    if (e.id.empty()) return std::nullopt;
    const Disposition d = classify(rules, item);
    if (disposition) *disposition = d;
    e.change_key = text_of(item, "changeKey");
    if (const auto s = item.find("start"); s != item.end()) e.start_at = graph_time(*s).value_or(0);
    if (const auto s = item.find("end"); s != item.end()) e.end_at = graph_time(*s).value_or(0);
    e.all_day = bool_of(item, "isAllDay");
    e.cancelled = bool_of(item, "isCancelled");
    e.show_as = text_of(item, "showAs");
    e.sensitivity = text_of(item, "sensitivity");
    e.label = item_label(item);
    e.series_master_id = text_of(item, "seriesMasterId");
    if (d != Disposition::include) {
        e.withheld = true;
        return e;
    }
    if (masked_private(rules, item)) {
        e.masked = true;
        return e;
    }
    e.ical_uid = text_of(item, "iCalUId");
    e.subject = text_of(item, "subject");
    e.body = body_of(item);
    if (const auto l = item.find("location"); l != item.end() && l->is_object()) e.location = text_of(*l, "displayName");
    if (const auto o = item.find("organizer"); o != item.end()) e.organizer = person_of(*o);
    if (const auto a = item.find("attendees"); a != item.end() && a->is_array())
        for (const auto& x : *a) {
            const Person p = person_of(x);
            if (p.address.empty()) continue;
            std::string response;
            if (const auto st = x.find("status"); st != x.end() && st->is_object()) response = text_of(*st, "response");
            e.attendees.push_back({{"name", p.name}, {"address", p.address}, {"type", text_of(x, "type")}, {"response", response}});
        }
    e.is_organizer = bool_of(item, "isOrganizer");
    if (const auto r = item.find("responseStatus"); r != item.end() && r->is_object()) e.response = text_of(*r, "response");
    e.categories = categories_of(item);
    e.web_link = text_of(item, "webLink");
    if (const auto om = item.find("onlineMeeting"); om != item.end() && om->is_object()) e.online_url = text_of(*om, "joinUrl");
    if (e.online_url.empty()) e.online_url = text_of(item, "onlineMeetingUrl");
    return e;
}

json message_json(const StoredMessage& m, bool with_body) {
    if (m.withheld)
        return {{"id", m.id}, {"withheld", "withheld (sensitivity label)"}, {"received", m.received_at ? iso8601_utc(sys_clock::time_point(std::chrono::milliseconds(m.received_at))) : ""}};
    json j = {{"id", m.id},
              {"subject", m.subject},
              {"from", person_json(m.from)},
              {"to", people_json(m.to)},
              {"received", iso8601_utc(sys_clock::time_point(std::chrono::milliseconds(m.received_at)))},
              {"conversation_id", m.conversation_id},
              {"has_attachments", m.has_attachments},
              {"is_read", m.is_read}};
    if (!m.cc.empty()) j["cc"] = people_json(m.cc);
    if (m.is_draft) j["draft"] = true;
    if (!m.categories.empty()) j["categories"] = m.categories;
    if (with_body) j["body"] = m.body;
    else j["preview"] = m.preview;
    return j;
}

json event_json(const StoredEvent& e, bool with_body) {
    const auto t = [](long long ms) { return iso8601_utc(sys_clock::time_point(std::chrono::milliseconds(ms))); };
    if (e.withheld) return {{"id", e.id}, {"withheld", "withheld (sensitivity label)"}, {"start", t(e.start_at)}, {"end", t(e.end_at)}};
    if (e.masked) return {{"id", e.id}, {"private", true}, {"show_as", e.show_as.empty() ? "busy" : e.show_as}, {"start", t(e.start_at)}, {"end", t(e.end_at)}};
    json j = {{"id", e.id},
              {"subject", e.subject},
              {"start", t(e.start_at)},
              {"end", t(e.end_at)},
              {"all_day", e.all_day},
              {"location", e.location},
              {"organizer", person_json(e.organizer)},
              {"attendees", e.attendees},
              {"is_organizer", e.is_organizer},
              {"response", e.response},
              {"show_as", e.show_as}};
    if (e.cancelled) j["cancelled"] = true;
    if (!e.series_master_id.empty()) j["series_master_id"] = e.series_master_id;
    if (!e.online_url.empty()) j["online_meeting_url"] = e.online_url;
    if (with_body) j["body"] = e.body;
    return j;
}

}  // namespace wb::mail
