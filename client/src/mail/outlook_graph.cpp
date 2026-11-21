#include "mail/outlook_graph.h"

#include "base/encoding.h"
#include "base/strings.h"
#include "base/url.h"
#include "mail/filter.h"

#include <algorithm>
#include <set>

namespace wb::mail::outlook {
namespace {

constexpr int kDefaultPage = 50;
constexpr char kDeltaPrefix[] = "outlook-delta|";
constexpr char kPagePrefix[] = "outlook-page|";

std::string iso(long long ms) { return iso8601_utc(sys_clock::time_point(std::chrono::milliseconds(ms))); }

json address_json(const OlAddress& a) { return {{"emailAddress", {{"name", a.name}, {"address", a.address}}}}; }

json label_props(const std::string& msip) {
    if (msip.empty()) return json::array();
    return json::array({{{"id", kLabelProperty}, {"value", msip}}});
}

std::vector<std::string> split_on(const std::string& s, char sep) {
    std::vector<std::string> out;
    size_t pos = 0;
    while (true) {
        const size_t e = s.find(sep, pos);
        out.push_back(s.substr(pos, e == std::string::npos ? std::string::npos : e - pos));
        if (e == std::string::npos) break;
        pos = e + 1;
    }
    return out;
}

std::string q1(const std::multimap<std::string, std::string>& q, const std::string& key) {
    const auto it = q.find(key);
    return it == q.end() ? std::string{} : it->second;
}

std::optional<long long> to_ms(const std::string& s) {
    if (s.empty()) return std::nullopt;
    if (auto v = parse_iso8601_ms(s)) return v;
    // Graph's dateTime without a zone is UTC (the client never asks for another zone).
    if (s.find('Z') == std::string::npos && s.find('+') == std::string::npos) return parse_iso8601_ms(s + "Z");
    return std::nullopt;
}

std::optional<long long> time_of(const json& v) {
    if (v.is_string()) return to_ms(v.get<std::string>());
    if (v.is_object()) return to_ms(jstr(v, "dateTime"));
    return std::nullopt;
}

std::vector<std::string> addresses(const json& body, const char* key) {
    std::vector<std::string> out;
    const auto it = body.find(key);
    if (it == body.end() || !it->is_array()) return out;
    for (const auto& r : *it) {
        const json* e = jobj(r, "emailAddress");
        if (e && !jstr(*e, "address").empty()) out.push_back(jstr(*e, "address"));
    }
    return out;
}

DraftPatch draft_patch(const json& b) {
    DraftPatch p;
    if (b.contains("subject")) p.subject = jstr(b, "subject");
    if (const json* body = jobj(b, "body")) p.body = jstr(*body, "content");
    if (b.contains("toRecipients")) p.to = addresses(b, "toRecipients");
    if (b.contains("ccRecipients")) p.cc = addresses(b, "ccRecipients");
    return p;
}

EventPatch event_patch(const json& b) {
    EventPatch p;
    if (b.contains("subject")) p.subject = jstr(b, "subject");
    if (const json* body = jobj(b, "body")) p.body = jstr(*body, "content");
    if (const json* l = jobj(b, "location")) p.location = jstr(*l, "displayName");
    if (b.contains("start")) p.start = time_of(b["start"]);
    if (b.contains("end")) p.end = time_of(b["end"]);
    if (const auto a = b.find("attendees"); a != b.end() && a->is_array()) {
        std::vector<std::string> req, opt;
        for (const auto& x : *a) {
            const json* e = jobj(x, "emailAddress");
            if (!e || jstr(*e, "address").empty()) continue;
            (jstr(x, "type") == "optional" ? opt : req).push_back(jstr(*e, "address"));
        }
        p.required = req;
        p.optional = opt;
    }
    return p;
}

Error bad(const std::string& why) { return make_error("bad_request", why); }

// The OData $filter subset the mail tools emit: conversationId eq '...', receivedDateTime ge|lt <iso>, hasAttachments eq true.
Status apply_filter(const std::string& filter, MessageQuery& mq) {
    std::string rest = filter;
    std::vector<std::string> parts;
    for (size_t pos = 0;;) {
        const size_t e = rest.find(" and ", pos);
        parts.push_back(rest.substr(pos, e == std::string::npos ? std::string::npos : e - pos));
        if (e == std::string::npos) break;
        pos = e + 5;
    }
    for (auto p : parts) {
        p = trim(p);
        const auto words = split_on(p, ' ');
        if (words.size() < 3) return bad("unsupported $filter: " + p);
        const std::string field = words[0], op = words[1];
        std::string value = p.substr(field.size() + op.size() + 2);
        if (value.size() >= 2 && value.front() == '\'' && value.back() == '\'') value = value.substr(1, value.size() - 2);
        if (field == "conversationId" && op == "eq") mq.conversation_id = value;
        else if (field == "receivedDateTime" && (op == "ge" || op == "gt")) mq.since = to_ms(value).value_or(0);
        else if (field == "receivedDateTime" && (op == "lt" || op == "le")) mq.until = to_ms(value).value_or(0);
        else if (field == "hasAttachments" && op == "eq") mq.attachments_only = value == "true";
        else return bad("unsupported $filter: " + p);
    }
    return {};
}

std::string to_hex_key(const std::map<std::string, std::string>& m) {
    json j = json::object();
    for (const auto& [k, v] : m) j[k] = v;
    return j.dump();
}

}  // namespace

std::string occurrence_id(const std::string& series_id, const std::string& local_stamp) { return series_id + "." + local_stamp; }

std::optional<std::pair<std::string, std::string>> split_occurrence_id(const std::string& id) {
    const size_t dot = id.rfind('.');
    if (dot == std::string::npos || dot == 0 || id.size() - dot - 1 != 13 || id[dot + 9] != 'T') return std::nullopt;
    return std::make_pair(id.substr(0, dot), id.substr(dot + 1));
}

json message_to_graph(const OlMessage& m) {
    json to = json::array(), cc = json::array();
    for (const auto& r : m.recipients) {
        if (r.type == "cc") cc.push_back(address_json(r.who));
        else if (r.type == "to") to.push_back(address_json(r.who));
    }
    std::string preview = m.body.substr(0, std::min<size_t>(m.body.size(), 255));
    preview = utf8_truncate(preview, 255);
    json j = {{"id", m.id},
              {"parentFolderId", m.folder_id},
              {"conversationId", m.conversation_id},
              {"changeKey", std::to_string(m.modified)},
              {"receivedDateTime", iso(m.received)},
              {"lastModifiedDateTime", iso(m.modified)},
              {"subject", m.subject},
              {"from", address_json(m.from)},
              {"sender", address_json(m.from)},
              {"toRecipients", to},
              {"ccRecipients", cc},
              {"body", {{"contentType", "text"}, {"content", m.body}}},
              {"bodyPreview", preview},
              {"hasAttachments", m.has_attachments},
              {"isRead", m.is_read},
              {"isDraft", m.is_draft},
              {"categories", m.categories},
              {"webLink", ""}};
    if (!m.msip_labels.empty()) j["singleValueExtendedProperties"] = label_props(m.msip_labels);
    return j;
}

std::string change_key_of(const OlEvent& e) { return std::to_string(e.modified) + "/" + std::to_string(e.start) + "/" + std::to_string(e.end); }

json event_to_graph(const OlEvent& e) {
    json att = json::array();
    for (const auto& a : e.attendees)
        att.push_back({{"emailAddress", {{"name", a.who.name}, {"address", a.who.address}}},
                       {"type", a.type},
                       {"status", {{"response", a.response.empty() ? "none" : a.response}}}});
    json j = {{"id", e.id},
              {"iCalUId", e.global_id},
              {"changeKey", change_key_of(e)},
              {"type", e.series_id.empty() ? "singleInstance" : "occurrence"},
              {"start", {{"dateTime", iso8601_graph(e.start)}, {"timeZone", "UTC"}}},
              {"end", {{"dateTime", iso8601_graph(e.end)}, {"timeZone", "UTC"}}},
              {"isAllDay", e.all_day},
              {"isCancelled", e.cancelled},
              {"isOrganizer", e.is_organizer},
              {"showAs", e.show_as.empty() ? "busy" : e.show_as},
              {"sensitivity", e.sensitivity.empty() ? "normal" : e.sensitivity},
              {"subject", e.subject},
              {"body", {{"contentType", "text"}, {"content", e.body}}},
              {"location", {{"displayName", e.location}}},
              {"organizer", address_json(e.organizer)},
              {"attendees", att},
              {"responseStatus", {{"response", e.response.empty() ? "none" : e.response}}},
              {"categories", e.categories},
              {"webLink", ""}};
    if (!e.series_id.empty()) j["seriesMasterId"] = e.series_id;
    if (!e.online_url.empty()) {
        j["isOnlineMeeting"] = true;
        j["onlineMeeting"] = {{"joinUrl", e.online_url}};
    }
    if (!e.msip_labels.empty()) j["singleValueExtendedProperties"] = label_props(e.msip_labels);
    return j;
}

OutlookGraph::OutlookGraph(OutlookSession& session, const Clock& clock, Persist persist)
    : s_(session), clock_(clock), persist_(std::move(persist)) {
    if (persist_.load)
        if (auto g = persist_.load("generation")) generation_ = *g;
    if (generation_.empty()) {
        generation_ = std::to_string(clock_.unix_millis());
        if (persist_.save) persist_.save("generation", generation_);
    }
}

void OutlookGraph::reset() {
    const std::lock_guard lock(mx_);
    generation_ = std::to_string(clock_.unix_millis() + next_seq_++);
    snapshots_.clear();
    sequences_.clear();
    if (persist_.save) persist_.save("generation", generation_);
}

std::map<std::string, std::string> OutlookGraph::load_snapshot(const std::string& key) {
    if (const auto it = snapshots_.find(key); it != snapshots_.end()) return it->second;
    std::map<std::string, std::string> out;
    if (persist_.load)
        if (auto v = persist_.load("snapshot:" + key))
            if (auto j = parse_json(*v); j && j->is_object())
                for (const auto& [k, x] : j->items())
                    if (x.is_string()) out[k] = x.get<std::string>();
    snapshots_[key] = out;
    return out;
}

void OutlookGraph::save_snapshot(const std::string& key, const std::map<std::string, std::string>& snap) {
    snapshots_[key] = snap;
    if (persist_.save) persist_.save("snapshot:" + key, to_hex_key(snap));
}

std::string OutlookGraph::delta_link(const std::string& key, const std::string& a, const std::string& b) const {
    return std::string(kDeltaPrefix) + generation_ + "|" + key + "|" + a + "|" + b;
}

Result<json> OutlookGraph::handle(const std::string& method, const std::string& path, const std::optional<json>& body, int page_size) {
    const std::lock_guard lock(mx_);
    if (page_size <= 0) page_size = kDefaultPage;
    try {
        return dispatch(method, path, body, page_size);
    } catch (const std::exception& ex) {
        // A malformed link or number: never a crash, and never a guess about the caller's state.
        return bad(std::string("malformed request: ") + ex.what());
    }
}

Result<json> OutlookGraph::dispatch(const std::string& method, const std::string& path, const std::optional<json>& body, int page_size) {
    if (path.rfind(kPagePrefix, 0) == 0) {
        const auto parts = split_on(path, '|');
        if (parts.size() != 3 || parts[1] != generation_) return make_error("sync_state_not_found", "syncStateNotFound: the page belongs to an earlier session");
        return next_page(parts[2], page_size);
    }
    if (path.rfind(kDeltaPrefix, 0) == 0) {
        const auto parts = split_on(path, '|');
        // outlook-delta|<generation>|folder|<id>|<since> or outlook-delta|<generation>|calendar|<from>|<to>
        if (parts.size() != 5 || parts[1] != generation_) return make_error("sync_state_not_found", "syncStateNotFound: the delta token has expired");
        if (parts[2] == "folder") return folder_delta(parts[3], std::stoll(parts[4].empty() ? "0" : parts[4]), false, page_size);
        if (parts[2] == "calendar") return calendar_delta(std::stoll(parts[3]), std::stoll(parts[4]), false, page_size);
        return make_error("sync_state_not_found", "syncStateNotFound");
    }
    const size_t qm = path.find('?');
    std::string p = path.substr(0, qm);
    const auto q = parse_query(qm == std::string::npos ? std::string_view{} : std::string_view(path).substr(qm + 1));
    if (p.empty() || p[0] != '/') p = "/" + p;
    return route(method, p, q, body, page_size);
}

Result<std::string> OutlookGraph::folder_ref(const std::string& id_or_name) {
    auto fs = s_.folders();
    if (!fs) return fs.error();
    const std::string lower = to_lower(id_or_name);
    for (const auto& f : *fs)
        if (f.id == id_or_name || (!f.well_known.empty() && f.well_known == lower) || to_lower(f.name) == lower) return f.id;
    return make_error("not_found", "ErrorItemNotFound: no such folder");
}

Result<json> OutlookGraph::folders() {
    auto fs = s_.folders();
    if (!fs) return fs.error();
    json v = json::array();
    for (const auto& f : *fs) {
        json j = {{"id", f.id}, {"displayName", f.name}};
        if (!f.well_known.empty()) j["wellKnownName"] = f.well_known;
        v.push_back(std::move(j));
    }
    return json{{"value", v}};
}

Result<json> OutlookGraph::folder_delta(const std::string& folder_id, long long since, bool initial, int page_size) {
    const std::string key = "folder:" + folder_id;
    auto rows = s_.rows(folder_id, since);
    if (!rows) return rows.error();
    const auto old = initial ? std::map<std::string, std::string>{} : load_snapshot(key);
    Sequence seq;
    seq.key = key;
    seq.folder_id = folder_id;
    for (const auto& r : *rows) {
        const std::string ck = std::to_string(r.modified);
        seq.snapshot[r.id] = ck;
        if (initial) {
            seq.to_fetch.push_back(r.id);
            continue;
        }
        const auto it = old.find(r.id);
        if (it == old.end() || it->second != ck) seq.to_fetch.push_back(r.id);
    }
    if (!initial)
        for (const auto& [id, ck] : old)
            if (!seq.snapshot.count(id)) seq.ready.push_back({{"id", id}, {"@removed", {{"reason", "deleted"}}}});
    seq.delta_link = delta_link("folder", folder_id, std::to_string(since));
    const std::string seq_id = std::to_string(next_seq_++);
    sequences_[seq_id] = std::move(seq);
    return next_page(seq_id, page_size);
}

Result<json> OutlookGraph::calendar_delta(long long from, long long to, bool initial, int page_size) {
    const std::string key = "calendar";
    auto events = s_.events(from, to, kMaxCalendarItems);
    if (!events) return events.error();
    const auto old = initial ? std::map<std::string, std::string>{} : load_snapshot(key);
    Sequence seq;
    seq.key = key;
    for (const auto& e : *events) {
        // The Jet restriction parses dates in the user's locale; anything outside the window in UTC is dropped here.
        if (e.end <= from || e.start >= to) continue;
        const std::string ck = change_key_of(e);
        if (seq.snapshot.count(e.id)) continue;  // an occurrence listed twice
        seq.snapshot[e.id] = ck;
        const auto it = old.find(e.id);
        if (initial || it == old.end() || it->second != ck) seq.ready.push_back(event_to_graph(e));
    }
    if (!initial)
        for (const auto& [id, ck] : old)
            if (!seq.snapshot.count(id)) seq.ready.push_back({{"id", id}, {"@removed", {{"reason", "deleted"}}}});
    seq.delta_link = delta_link("calendar", std::to_string(from), std::to_string(to));
    const std::string seq_id = std::to_string(next_seq_++);
    sequences_[seq_id] = std::move(seq);
    return next_page(seq_id, page_size);
}

Result<json> OutlookGraph::next_page(const std::string& seq_id, int page_size) {
    const auto it = sequences_.find(seq_id);
    if (it == sequences_.end()) return make_error("sync_state_not_found", "syncStateNotFound: unknown page");
    Sequence& seq = it->second;
    json value = json::array();
    const size_t want = static_cast<size_t>(page_size);
    while (value.size() < want && !seq.ready.empty()) {
        value.push_back(std::move(seq.ready.back()));
        seq.ready.pop_back();
    }
    while (value.size() < want && !seq.to_fetch.empty()) {
        const std::string id = seq.to_fetch.back();
        seq.to_fetch.pop_back();
        auto m = s_.message(id);
        if (!m) {
            if (m.error().code == "not_found") {
                // Gone between the table read and now: removed, and not part of the next snapshot.
                seq.snapshot.erase(id);
                value.push_back({{"id", id}, {"@removed", {{"reason", "deleted"}}}});
                continue;
            }
            seq.to_fetch.push_back(id);  // retried by the next attempt of this page
            return m.error();
        }
        if (m->folder_id.empty()) m->folder_id = seq.folder_id;
        value.push_back(message_to_graph(*m));
    }
    json out = {{"value", value}};
    if (seq.ready.empty() && seq.to_fetch.empty()) {
        save_snapshot(seq.key, seq.snapshot);
        out["@odata.deltaLink"] = seq.delta_link;
        sequences_.erase(it);
    } else {
        out["@odata.nextLink"] = std::string(kPagePrefix) + generation_ + "|" + seq_id;
    }
    return out;
}

Result<json> OutlookGraph::list_messages(const std::string& folder_id, const std::multimap<std::string, std::string>& q) {
    MessageQuery mq;
    mq.folder_id = folder_id;
    if (const std::string s = q1(q, "$search"); !s.empty()) {
        std::string t = s;
        std::erase(t, '"');
        mq.text = trim(t);
    }
    if (const std::string f = q1(q, "$filter"); !f.empty())
        if (Status st = apply_filter(f, mq); !st) return st.error();
    if (const std::string top = q1(q, "$top"); !top.empty()) mq.limit = std::clamp<size_t>(std::strtoul(top.c_str(), nullptr, 10), 1, 200);
    auto found = s_.find(mq);
    if (!found) return found.error();
    std::sort(found->begin(), found->end(), [](const OlMessage& a, const OlMessage& b) { return a.received > b.received; });
    json v = json::array();
    for (const auto& m : *found) {
        if (v.size() >= mq.limit) break;
        v.push_back(message_to_graph(m));
    }
    return json{{"value", v}};
}

Result<json> OutlookGraph::get_schedule(const json& body) {
    const auto from = body.contains("startTime") ? time_of(body["startTime"]) : std::nullopt;
    const auto to = body.contains("endTime") ? time_of(body["endTime"]) : std::nullopt;
    const int interval = static_cast<int>(jint(body, "availabilityViewInterval", 30));
    if (!from || !to || *to <= *from || interval < 5 || interval > 1440) return bad("getSchedule needs startTime, endTime and an interval");
    const int slots = static_cast<int>((*to - *from) / (interval * 60'000LL));
    json v = json::array();
    if (const auto s = body.find("schedules"); s != body.end() && s->is_array())
        for (const auto& x : *s) {
            if (!x.is_string()) continue;
            const std::string who = x.get<std::string>();
            auto fb = s_.free_busy(who, *from, interval, slots);
            if (!fb) {
                if (fb.error().code == "offline" || fb.error().code == "throttled" || fb.error().code == "cancelled") return fb.error();
                v.push_back({{"scheduleId", who}, {"error", {{"message", fb.error().message}, {"responseCode", fb.error().code}}}});
                continue;
            }
            v.push_back({{"scheduleId", who}, {"availabilityView", *fb}});
        }
    return json{{"value", v}};
}

Result<json> OutlookGraph::create_event(const json& body) {
    const long long now = clock_.unix_millis();
    for (auto it = transactions_.begin(); it != transactions_.end();)
        it = now - it->second.second > kTransactionTtlMs ? transactions_.erase(it) : std::next(it);
    const std::string tx = jstr(body, "transactionId");
    if (!tx.empty())
        if (const auto it = transactions_.find(tx); it != transactions_.end()) {
            // A retried create: the event made by the first attempt, no second invitation (Graph transactionId).
            auto e = s_.event(it->second.first);
            if (e) return event_to_graph(*e);
        }
    auto e = s_.save_event("", event_patch(body));
    if (!e) return e.error();
    if (!tx.empty()) transactions_[tx] = {e->id, now};
    return event_to_graph(*e);
}

Result<json> OutlookGraph::route(const std::string& method, const std::string& path, const std::multimap<std::string, std::string>& q,
                                 const std::optional<json>& body, int page_size) {
    const json b = body && body->is_object() ? *body : json::object();
    auto seg = split_on(path.substr(1), '/');
    for (auto& s : seg) s = url_decode(s);
    if (seg.empty() || seg[0] != "me") return bad("only /me is available on the Local Outlook backend");
    const size_t n = seg.size();

    if (n == 1 && method == "GET") {
        auto me = s_.me();
        if (!me) return me.error();
        return json{{"displayName", me->name}, {"mail", me->address}, {"userPrincipalName", me->address}};
    }
    if (n < 2) return bad("unsupported request");
    // ---- folders ----
    if (seg[1] == "mailFolders") {
        if (n == 2 && method == "GET") return folders();
        if (n >= 4 && seg[3] == "messages") {
            auto fid = folder_ref(seg[2]);
            if (!fid) return fid.error();
            if (n == 5 && seg[4] == "delta" && method == "GET") {
                MessageQuery mq;
                if (const std::string f = q1(q, "$filter"); !f.empty())
                    if (Status st = apply_filter(f, mq); !st) return st.error();
                return folder_delta(*fid, mq.since, true, page_size);
            }
            if (n == 4 && method == "GET") return list_messages(*fid, q);
        }
        return bad("unsupported mail folder request");
    }
    // ---- messages ----
    if (seg[1] == "sendMail" && n == 2 && method == "POST") {
        const json* m = jobj(b, "message");
        if (!m) return bad("sendMail needs a message");
        if (Status st = s_.send_new(draft_patch(*m)); !st) return st.error();
        return json(nullptr);
    }
    if (seg[1] == "messages") {
        if (n == 2 && method == "GET") return list_messages({}, q);
        if (n == 2 && method == "POST") {
            auto m = s_.create_draft(draft_patch(b));
            if (!m) return m.error();
            return message_to_graph(*m);
        }
        if (n < 3) return bad("unsupported message request");
        const std::string id = seg[2];
        if (n == 3) {
            if (method == "GET") {
                auto m = s_.message(id);
                if (!m) return m.error();
                return message_to_graph(*m);
            }
            if (method == "PATCH") {
                auto m = s_.update_draft(id, draft_patch(b));
                if (!m) return m.error();
                return message_to_graph(*m);
            }
            if (method == "DELETE") {
                if (Status st = s_.remove(id); !st) return st.error();
                return json(nullptr);
            }
        }
        if (n < 4) return bad("unsupported message request");
        const std::string action = seg[3];
        if (action == "attachments" && method == "GET") {
            if (n == 4) {
                auto list = s_.attachments(id);
                if (!list) return list.error();
                json v = json::array();
                for (const auto& a : *list) v.push_back({{"id", a.id}, {"name", a.name}, {"contentType", a.content_type}, {"size", a.size}});
                return json{{"value", v}};
            }
            if (n == 5) {
                auto list = s_.attachments(id);
                if (!list) return list.error();
                const auto a = std::find_if(list->begin(), list->end(), [&](const OlAttachment& x) { return x.id == seg[4]; });
                if (a == list->end()) return make_error("not_found", "ErrorItemNotFound: no such attachment");
                auto bytes = s_.attachment_bytes(id, seg[4]);
                if (!bytes) return bytes.error();
                return json{{"@odata.type", "#microsoft.graph.fileAttachment"},
                            {"id", a->id},
                            {"name", a->name},
                            {"contentType", a->content_type},
                            {"size", a->size},
                            {"contentBytes", base64_encode(reinterpret_cast<const std::uint8_t*>(bytes->data()), bytes->size())}};
            }
        }
        if (n == 4 && method == "POST") {
            if (action == "createReply" || action == "createReplyAll" || action == "createForward") {
                const std::string kind = action == "createReply" ? "reply" : action == "createReplyAll" ? "reply_all" : "forward";
                auto m = s_.create_reply(id, kind, jstr(b, "comment"));
                if (!m) return m.error();
                if (b.contains("toRecipients")) {
                    DraftPatch p;
                    p.to = addresses(b, "toRecipients");
                    m = s_.update_draft(m->id, p);
                    if (!m) return m.error();
                }
                return message_to_graph(*m);
            }
            if (action == "send") {
                if (Status st = s_.send(id); !st) return st.error();
                return json(nullptr);
            }
            if (action == "move") {
                auto fid = folder_ref(jstr(b, "destinationId"));
                if (!fid) return fid.error();
                auto m = s_.move(id, *fid);
                if (!m) return m.error();
                return message_to_graph(*m);
            }
        }
        return bad("unsupported message request");
    }
    // ---- calendar ----
    if (seg[1] == "calendarView" && method == "GET") {
        const auto from = to_ms(q1(q, "startDateTime")), to = to_ms(q1(q, "endDateTime"));
        if (!from || !to || *to <= *from) return bad("calendarView needs startDateTime and endDateTime");
        if (n == 3 && seg[2] == "delta") return calendar_delta(*from, *to, true, page_size);
        if (n == 2) {
            auto events = s_.events(*from, *to, kMaxCalendarItems);
            if (!events) return events.error();
            json v = json::array();
            for (const auto& e : *events)
                if (e.end > *from && e.start < *to) v.push_back(event_to_graph(e));
            return json{{"value", v}};
        }
    }
    if (seg[1] == "calendar" && n == 3 && seg[2] == "getSchedule" && method == "POST") return get_schedule(b);
    if (seg[1] == "events") {
        if (n == 2 && method == "POST") return create_event(b);
        if (n == 3 && method == "GET") {
            auto e = s_.event(seg[2]);
            if (!e) return e.error();
            return event_to_graph(*e);
        }
        if (n == 3 && method == "PATCH") {
            auto e = s_.save_event(seg[2], event_patch(b));
            if (!e) return e.error();
            return event_to_graph(*e);
        }
        if (n == 4 && method == "POST") {
            const std::string a = seg[3];
            const std::string r = a == "accept" ? "accept" : a == "tentativelyAccept" ? "tentative" : a == "decline" ? "decline" : "";
            if (r.empty()) return bad("unsupported event action");
            if (Status st = s_.respond(seg[2], r, jstr(b, "comment")); !st) return st.error();
            return json(nullptr);
        }
    }
    return bad("unsupported request on the Local Outlook backend: " + method + " " + path);
}

}  // namespace wb::mail::outlook
