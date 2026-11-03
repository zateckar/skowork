// The mail and calendar tools (client/mail-calendar Mail Tools And Risk Classes, Send Always Requires Approval, Calendar
// Tools And Risk Classes, Sensitivity Label Exclusion, Offline Mail Behaviour; design D2). Reads go to Graph and fall
// back to the local copy when Graph is unreachable; everything that changes the mailbox fails with "offline" then.
// Mail content is untrusted (provenance mail).
#include "base/encoding.h"
#include "base/strings.h"
#include "base/url.h"
#include "mail/graph_parse.h"
#include "mail/mail_service.h"
#include "tools/text.h"

#include <algorithm>
#include <ctime>

namespace wb::mail {
namespace {

using runtime::Risk;
using runtime::ToolContext;
using runtime::ToolDef;
using runtime::ToolOutput;

constexpr size_t kMaxAttachmentText = 200'000;
constexpr long long kDayMs = 86'400'000;

std::string iso(long long ms) { return iso8601_utc(sys_clock::time_point(std::chrono::milliseconds(ms))); }

// The computer's local time with its offset ("2026-10-27T09:00:00+01:00"): the user's time zone for free slots.
std::string local_iso(long long ms) {
    const std::time_t t = static_cast<std::time_t>(ms / 1000);
    std::tm local{};
    localtime_s(&local, &t);
    std::tm copy = local;
    const long long offset = static_cast<long long>(_mkgmtime(&copy)) - static_cast<long long>(t);
    char buf[40];
    const long long oh = (offset < 0 ? -offset : offset) / 3600, om = ((offset < 0 ? -offset : offset) % 3600) / 60;
    std::snprintf(buf, sizeof buf, "%04d-%02d-%02dT%02d:%02d:%02d%c%02lld:%02lld", local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
                  local.tm_hour, local.tm_min, local.tm_sec, offset < 0 ? '-' : '+', oh, om);
    return buf;
}

ToolOutput fail(const Error& e) {
    if (e.code == "offline")
        return ToolOutput::failure("offline: Microsoft Graph cannot be reached, so this was not done. (" + e.message + ")");
    if (e.code == "not_connected")
        return ToolOutput::failure("The mailbox is not connected. The user can connect it in Settings > Mail and calendar.");
    if (e.code == "interaction_required")
        return ToolOutput::failure("The mailbox needs the user to sign in again; a Connect mailbox question is in their inbox.");
    if (e.code == "unavailable" || e.code == "not_configured" || e.code == "policy_error")
        return ToolOutput::failure("Mail and calendar are not available: " + e.message);
    if (e.code == "not_found") return ToolOutput::failure("Not found: the item does not exist or was moved.");
    return ToolOutput::failure(e.code + ": " + e.message);
}

runtime::Provenance mail_provenance(const std::string& id) { return {"mail", id, iso8601_utc_now(), false}; }

json string_list(const json& args, const char* key) {
    json out = json::array();
    if (const auto it = args.find(key); it != args.end()) {
        if (it->is_string()) out.push_back(it->get<std::string>());
        else if (it->is_array())
            for (const auto& x : *it)
                if (x.is_string() && !x.get<std::string>().empty()) out.push_back(x.get<std::string>());
    }
    return out;
}

json recipients(const json& addresses) {
    json out = json::array();
    for (const auto& a : addresses) out.push_back({{"emailAddress", {{"address", a.get<std::string>()}}}});
    return out;
}

std::optional<long long> time_arg(const json& args, const char* key) {
    const std::string v = jstr(args, key);
    if (v.empty()) return std::nullopt;
    return parse_iso8601_ms(v);
}

json schema(json props, std::vector<std::string> required = {}) {
    json s = {{"type", "object"}, {"properties", std::move(props)}, {"additionalProperties", false}};
    if (!required.empty()) s["required"] = required;
    return s;
}

const json kAddresses = {{"type", "array"}, {"items", {{"type", "string"}}}, {"description", "Email addresses"}};
const json kTime = {{"type", "string"}, {"description", "ISO 8601 date-time with its offset, e.g. 2026-10-27T09:00:00+01:00"}};

class Tools {
public:
    explicit Tools(MailService& svc) : svc_(svc) {}

    // The connection, or the reason there is none usable.
    Result<std::shared_ptr<Connection>> conn() {
        auto c = svc_.connection();
        if (!c) return make_error("unavailable", "mail is not started");
        if (!c->cfg.available) return make_error(c->cfg.unavailable_code.empty() ? "unavailable" : c->cfg.unavailable_code, c->cfg.unavailable_reason);
        return c;
    }

    GraphCall call(ToolContext& ctx, std::string path, std::string method = "GET") {
        GraphCall c;
        c.method = std::move(method);
        c.path = std::move(path);
        if (ctx.control) c.cancel = ctx.control->http_token();
        c.on_delay = [&ctx](long long ms, const std::string& why) {
            if (ctx.activity) ctx.activity(why + "; retrying in " + std::to_string((ms + 999) / 1000) + " s");
        };
        return c;
    }

    std::vector<std::string> folder_names(const std::string& folder_id) {
        if (auto f = svc_.store().folder(folder_id)) return {f->well_known, f->name};
        return {};
    }

    std::string folder_ref(const std::string& name) {
        if (auto f = svc_.store().folder(name)) return f->id;
        return name;  // Graph accepts well-known names (inbox, archive, deleteditems) in place of ids
    }

    // A message from Graph as the tools show it, or nullopt when it is excluded.
    std::optional<json> shown_message(const json& item, const Rules& rules, bool with_body) {
        Disposition d = Disposition::include;
        auto m = parse_message(item, rules, folder_names(jstr(item, "parentFolderId")), &d);
        if (!m || d == Disposition::excluded) return std::nullopt;
        return message_json(*m, with_body);
    }

    // ---- mail.search ----
    ToolOutput search(ToolContext& ctx, const json& a) {
        auto c = conn();
        if (!c) return fail(c.error());
        const Rules rules = svc_.rules();
        const std::string text = jstr(a, "query"), folder = jstr(a, "folder"), from = to_lower(jstr(a, "from"));
        const auto since = time_arg(a, "since"), until = time_arg(a, "until");
        const auto has_att = a.contains("has_attachments") && a["has_attachments"].is_boolean() ? std::optional<bool>(a["has_attachments"].get<bool>()) : std::nullopt;
        const int limit = std::clamp(static_cast<int>(jint(a, "limit", 20)), 1, 50);
        if (!folder.empty() && folder_excluded(rules, folder_names(folder_ref(folder))))
            return ToolOutput::failure("That folder is excluded from mail access.");
        std::string path = folder.empty() ? "/me/messages" : "/me/mailFolders/" + url_encode(folder_ref(folder)) + "/messages";
        std::vector<std::string> q;
        if (!text.empty()) {
            std::string s = text;
            std::erase(s, '"');
            q.push_back("$search=" + url_encode("\"" + s + "\""));
        } else {
            std::vector<std::string> f;
            if (since) f.push_back("receivedDateTime ge " + iso(*since));
            if (until) f.push_back("receivedDateTime lt " + iso(*until));
            if (has_att && *has_att) f.push_back("hasAttachments eq true");
            if (!f.empty()) {
                std::string joined;
                for (const auto& x : f) joined += (joined.empty() ? "" : " and ") + x;
                q.push_back("$filter=" + url_encode(joined));
            }
            q.push_back("$orderby=" + url_encode("receivedDateTime desc"));
        }
        q.push_back("$top=" + std::to_string(std::min(limit * 2, 100)));
        q.push_back("$expand=" + url_encode(label_expand()));
        for (size_t i = 0; i < q.size(); ++i) path += (i ? "&" : "?") + q[i];
        bool truncated = false;
        auto items = (*c)->graph->list(call(ctx, path), static_cast<size_t>(limit) * 4, &truncated);
        json results = json::array();
        if (!items) {
            if (items.error().code != "offline") return fail(items.error());
            // Offline: the local copy, labelled with the time of the last successful sync.
            SearchQuery sq;
            sq.text = text;
            if (!folder.empty())
                if (auto f = svc_.store().folder(folder)) sq.folder_id = f->id;
            sq.from = from;
            sq.since = since.value_or(0);
            sq.until = until.value_or(0);
            sq.has_attachments = has_att;
            sq.limit = limit;
            for (const auto& m : svc_.store().search(sq)) results.push_back(message_json(m, false));
            const long long last = svc_.store().last_sync();
            json out = {{"offline", true},
                        {"note", "Microsoft Graph is unreachable; these results come from the local copy and may be stale."},
                        {"last_sync", last ? iso(last) : "never"},
                        {"results", results}};
            ToolOutput o = ToolOutput::success(out.dump());
            o.provenance = mail_provenance("search");
            return o;
        }
        for (const auto& item : *items) {
            if (static_cast<int>(results.size()) >= limit) break;
            auto shown = shown_message(item, rules, false);
            if (!shown) continue;
            if (!shown->contains("withheld")) {
                const json* f = jobj(*shown, "from");
                if (!from.empty() && (!f || (to_lower(jstr(*f, "address")).find(from) == std::string::npos &&
                                             to_lower(jstr(*f, "name")).find(from) == std::string::npos)))
                    continue;
                if (has_att && jbool(*shown, "has_attachments", false) != *has_att) continue;
            } else if (!from.empty()) {
                continue;  // nothing is known about a withheld message's sender
            }
            const auto received = item.contains("receivedDateTime") ? graph_time(item["receivedDateTime"]) : std::nullopt;
            if (received && ((since && *received < *since) || (until && *received >= *until))) continue;
            results.push_back(std::move(*shown));
        }
        ToolOutput o = ToolOutput::success(json{{"results", results}, {"more", truncated}}.dump());
        o.provenance = mail_provenance("search");
        return o;
    }

    // ---- mail.read ----
    ToolOutput read(ToolContext& ctx, const json& a) {
        auto c = conn();
        if (!c) return fail(c.error());
        const Rules rules = svc_.rules();
        const std::string id = jstr(a, "id");
        if (id.empty()) return ToolOutput::failure("id is required");
        const std::string expand = "$expand=" + url_encode(label_expand());
        auto item = (*c)->graph->call(call(ctx, "/me/messages/" + url_encode(id) + "?" + expand));
        if (!item) {
            if (item.error().code != "offline") return fail(item.error());
            auto m = svc_.store().message(id);
            if (!m) return fail(item.error());
            if (m->withheld) return ToolOutput::success(message_json(*m, false).dump());
            json out = message_json(*m, true);
            out["offline"] = true;
            const long long last = svc_.store().last_sync();
            out["note"] = "Microsoft Graph is unreachable; this is the local copy as of " + (last ? iso(last) : std::string("the last sync")) + ".";
            ToolOutput o = ToolOutput::success(out.dump());
            o.provenance = mail_provenance(id);
            return o;
        }
        auto shown = shown_message(*item, rules, true);
        if (!shown) return ToolOutput::failure("That message is excluded from mail access.");
        if (shown->contains("withheld")) return ToolOutput::success(shown->dump());
        // The attachment's text, through Document Text Extraction for documents.
        if (const std::string aid = jstr(a, "attachment_id"); !aid.empty()) {
            auto att = (*c)->graph->call(call(ctx, "/me/messages/" + url_encode(id) + "/attachments/" + url_encode(aid)));
            if (!att) return fail(att.error());
            const std::string name = jstr(*att, "name");
            auto bytes = base64_decode(jstr(*att, "contentBytes"));
            if (!bytes) return ToolOutput::failure("The attachment has no readable content (it may be a link or an item attachment).");
            const std::string data(bytes->begin(), bytes->end());
            std::string text;
            if (const std::string kind = tools::document_kind(name); !kind.empty()) {
                if (!svc_.env().extract) return ToolOutput::failure("Document text extraction is not available.");
                auto t = svc_.env().extract(kind, data, ctx);
                if (!t) return ToolOutput::failure("The attachment's text could not be extracted: " + t.error().message);
                text = std::move(*t);
            } else if (auto d = tools::decode_text(data)) {
                text = std::move(d->text);
            } else {
                return ToolOutput::failure("The attachment is not text or a supported document (docx, xlsx, pptx, pdf).");
            }
            const bool cut = text.size() > kMaxAttachmentText;
            if (cut) text = utf8_truncate(text, kMaxAttachmentText);
            json out = {{"message_id", id}, {"attachment", {{"id", aid}, {"name", name}, {"content_type", jstr(*att, "contentType")}}}, {"text", text}};
            if (cut) out["truncated"] = true;
            ToolOutput o = ToolOutput::success(out.dump());
            o.provenance = mail_provenance(id + "/" + aid);
            return o;
        }
        json out = *shown;
        if (jbool(*item, "hasAttachments", false)) {
            auto list = (*c)->graph->list(call(ctx, "/me/messages/" + url_encode(id) + "/attachments?$select=id,name,contentType,size"), 50);
            if (list) {
                json atts = json::array();
                for (const auto& x : *list)
                    atts.push_back({{"id", jstr(x, "id")}, {"name", jstr(x, "name")}, {"content_type", jstr(x, "contentType")}, {"size", jint(x, "size", 0)}});
                out["attachments"] = atts;
            }
        }
        if (jbool(a, "thread", false)) {
            const std::string conv = jstr(*item, "conversationId");
            auto list = (*c)->graph->list(call(ctx, "/me/messages?$filter=" + url_encode("conversationId eq '" + conv + "'") + "&$top=50&" + expand), 50);
            if (!list) return fail(list.error());
            json thread = json::array();
            for (const auto& x : *list)
                if (auto s = shown_message(x, rules, true)) thread.push_back(std::move(*s));
            std::sort(thread.begin(), thread.end(), [](const json& l, const json& r) { return jstr(l, "received") < jstr(r, "received"); });
            out = {{"conversation_id", conv}, {"messages", thread}};
        }
        ToolOutput o = ToolOutput::success(out.dump());
        o.provenance = mail_provenance(id);
        return o;
    }

    // ---- mail.draft ----
    ToolOutput draft(ToolContext& ctx, const json& a) {
        auto c = conn();
        if (!c) return fail(c.error());
        const json to = string_list(a, "to"), cc = string_list(a, "cc");
        const std::string body = jstr(a, "body"), subject = jstr(a, "subject");
        std::string draft_id = jstr(a, "draft_id");
        Result<json> made = json(nullptr);
        if (draft_id.empty()) {
            if (const std::string of = jstr(a, "reply_to"); !of.empty()) {
                auto g = call(ctx, "/me/messages/" + url_encode(of) + (jbool(a, "reply_all", false) ? "/createReplyAll" : "/createReply"), "POST");
                g.body = json{{"comment", body}};
                made = (*c)->graph->call(g);
            } else if (const std::string fw = jstr(a, "forward_of"); !fw.empty()) {
                auto g = call(ctx, "/me/messages/" + url_encode(fw) + "/createForward", "POST");
                g.body = json{{"comment", body}, {"toRecipients", recipients(to)}};
                made = (*c)->graph->call(g);
            } else {
                auto g = call(ctx, "/me/messages", "POST");
                g.body = json{{"subject", subject}, {"body", {{"contentType", "text"}, {"content", body}}}, {"toRecipients", recipients(to)}, {"ccRecipients", recipients(cc)}};
                made = (*c)->graph->call(g);
            }
            if (!made) return fail(made.error());
            draft_id = jstr(*made, "id");
        }
        // Changes on top of a new reply or an existing draft.
        json patch = json::object();
        if (!jstr(a, "draft_id").empty() || !jstr(a, "reply_to").empty() || !jstr(a, "forward_of").empty()) {
            if (a.contains("to")) patch["toRecipients"] = recipients(to);
            if (a.contains("cc")) patch["ccRecipients"] = recipients(cc);
            if (a.contains("subject")) patch["subject"] = subject;
            if (!jstr(a, "draft_id").empty() && a.contains("body")) patch["body"] = {{"contentType", "text"}, {"content", body}};
        }
        if (!patch.empty()) {
            auto g = call(ctx, "/me/messages/" + url_encode(draft_id), "PATCH");
            g.body = patch;
            made = (*c)->graph->call(g);
            if (!made) return fail(made.error());
        }
        json out = {{"draft_id", draft_id}, {"saved_in", "Drafts"}, {"sent", false}};
        if (made && made->is_object()) {
            out["subject"] = jstr(*made, "subject");
            json rcpt = json::array();
            if (const auto r = made->find("toRecipients"); r != made->end() && r->is_array())
                for (const auto& x : *r) rcpt.push_back(person_of(x).address);
            out["to"] = rcpt;
        }
        return ToolOutput::success(out.dump());
    }

    // ---- mail.send ----
    ToolOutput send(ToolContext& ctx, const json& a) {
        auto c = conn();
        if (!c) return fail(c.error());
        const json to = string_list(a, "to"), cc = string_list(a, "cc");
        if (to.empty() && cc.empty()) return ToolOutput::failure("At least one recipient is required.");
        const std::string subject = jstr(a, "subject"), body = jstr(a, "body");
        const json text_body = {{"contentType", "text"}, {"content", body}};
        if (const std::string of = jstr(a, "reply_to"); !of.empty()) {
            // A reply keeps the conversation: a reply draft with exactly the approved recipients, subject and body, sent.
            auto g = call(ctx, "/me/messages/" + url_encode(of) + "/createReply", "POST");
            g.body = json::object();
            auto d = (*c)->graph->call(g);
            if (!d) return fail(d.error());
            const std::string id = jstr(*d, "id");
            auto p = call(ctx, "/me/messages/" + url_encode(id), "PATCH");
            p.body = json{{"subject", subject}, {"body", text_body}, {"toRecipients", recipients(to)}, {"ccRecipients", recipients(cc)}};
            if (auto r = (*c)->graph->call(p); !r) return fail(r.error());
            if (auto r = (*c)->graph->call(call(ctx, "/me/messages/" + url_encode(id) + "/send", "POST")); !r) return fail(r.error());
        } else {
            auto g = call(ctx, "/me/sendMail", "POST");
            g.body = json{{"message", {{"subject", subject}, {"body", text_body}, {"toRecipients", recipients(to)}, {"ccRecipients", recipients(cc)}}},
                          {"saveToSentItems", true}};
            if (auto r = (*c)->graph->call(g); !r) return fail(r.error());
        }
        return ToolOutput::success(json{{"sent", true}, {"to", to}, {"cc", cc}, {"subject", subject}}.dump());
    }

    // ---- mail.move / mail.delete ----
    ToolOutput move(ToolContext& ctx, const std::string& id, const std::string& folder) {
        auto c = conn();
        if (!c) return fail(c.error());
        if (id.empty() || folder.empty()) return ToolOutput::failure("id and folder are required");
        auto g = call(ctx, "/me/messages/" + url_encode(id) + "/move", "POST");
        g.body = json{{"destinationId", folder_ref(folder)}};
        auto r = (*c)->graph->call(g);
        if (!r) return fail(r.error());
        return ToolOutput::success(json{{"moved", true}, {"id", jstr(*r, "id")}, {"folder", folder}}.dump());
    }

    // ---- calendar ----
    std::optional<json> shown_event(const json& item, const Rules& rules, bool with_body) {
        Disposition d = Disposition::include;
        auto e = parse_event(item, rules, &d);
        if (!e || d == Disposition::excluded) return std::nullopt;
        return event_json(*e, with_body);
    }

    ToolOutput list_events(ToolContext& ctx, const json& a) {
        auto c = conn();
        if (!c) return fail(c.error());
        const Rules rules = svc_.rules();
        const long long now = svc_.env().clock->unix_millis();
        const long long from = time_arg(a, "start").value_or(now);
        const long long to = time_arg(a, "end").value_or(from + 7 * kDayMs);
        if (to <= from) return ToolOutput::failure("end must be after start");
        const int limit = std::clamp(static_cast<int>(jint(a, "limit", 50)), 1, 200);
        const std::string path = "/me/calendarView?startDateTime=" + url_encode(iso(from)) + "&endDateTime=" + url_encode(iso(to)) +
                                 "&$top=50&$expand=" + url_encode(label_expand());
        auto items = (*c)->graph->list(call(ctx, path), static_cast<size_t>(limit));
        json events = json::array();
        if (!items) {
            if (items.error().code != "offline") return fail(items.error());
            for (const auto& e : svc_.store().events_between(from, to))
                if (static_cast<int>(events.size()) < limit) events.push_back(event_json(e, false));
            const long long last = svc_.store().last_sync();
            ToolOutput o = ToolOutput::success(json{{"offline", true}, {"last_sync", last ? iso(last) : "never"}, {"events", events}}.dump());
            o.provenance = mail_provenance("calendar");
            return o;
        }
        for (const auto& item : *items)
            if (auto s = shown_event(item, rules, false)) events.push_back(std::move(*s));
        std::stable_sort(events.begin(), events.end(), [](const json& l, const json& r) { return jstr(l, "start") < jstr(r, "start"); });
        ToolOutput o = ToolOutput::success(json{{"events", events}}.dump());
        o.provenance = mail_provenance("calendar");
        return o;
    }

    ToolOutput get_event(ToolContext& ctx, const json& a) {
        auto c = conn();
        if (!c) return fail(c.error());
        const std::string id = jstr(a, "id");
        if (id.empty()) return ToolOutput::failure("id is required");
        auto item = (*c)->graph->call(call(ctx, "/me/events/" + url_encode(id) + "?$expand=" + url_encode(label_expand())));
        if (!item) {
            if (item.error().code != "offline") return fail(item.error());
            auto e = svc_.store().event(id);
            if (!e) return fail(item.error());
            json out = event_json(*e, true);
            out["offline"] = true;
            ToolOutput o = ToolOutput::success(out.dump());
            o.provenance = mail_provenance(id);
            return o;
        }
        auto shown = shown_event(*item, svc_.rules(), true);
        if (!shown) return ToolOutput::failure("That event is excluded from calendar access.");
        ToolOutput o = ToolOutput::success(shown->dump());
        o.provenance = mail_provenance(id);
        return o;
    }

    ToolOutput free_time(ToolContext& ctx, const json& a) {
        auto c = conn();
        if (!c) return fail(c.error());
        const long long now = svc_.env().clock->unix_millis();
        const int duration = static_cast<int>(jint(a, "duration_minutes", 30));
        if (duration < 5 || duration > 24 * 60) return ToolOutput::failure("duration_minutes must be between 5 and 1440");
        long long from = time_arg(a, "start").value_or(now);
        const long long to = time_arg(a, "end").value_or(from + 7 * kDayMs);
        if (to <= from || to - from > 62 * kDayMs) return ToolOutput::failure("the range must be positive and at most 62 days");
        const int interval = duration % 15 == 0 ? 15 : 5;
        from = (from + interval * 60'000LL - 1) / (interval * 60'000LL) * (interval * 60'000LL);  // on the grid
        std::string me = svc_.account();
        if (me.empty()) {
            if (auto j = (*c)->graph->call(call(ctx, "/me?$select=mail,userPrincipalName")); j) me = jstr(*j, "mail").empty() ? jstr(*j, "userPrincipalName") : jstr(*j, "mail");
            else return fail(j.error());
        }
        json schedules = json::array({me});
        for (const auto& x : string_list(a, "attendees"))
            if (to_lower(x.get<std::string>()) != to_lower(me)) schedules.push_back(x);
        auto g = call(ctx, "/me/calendar/getSchedule", "POST");
        g.body = json{{"schedules", schedules},
                      {"startTime", {{"dateTime", iso8601_graph(from)}, {"timeZone", "UTC"}}},
                      {"endTime", {{"dateTime", iso8601_graph(to)}, {"timeZone", "UTC"}}},
                      {"availabilityViewInterval", interval}};
        auto r = (*c)->graph->call(g);
        if (!r) return fail(r.error());
        std::vector<std::string> views;
        json unknown = json::array();
        if (const auto v = r->find("value"); v != r->end() && v->is_array())
            for (const auto& s : *v) {
                if (s.contains("error")) {
                    unknown.push_back(jstr(s, "scheduleId"));
                    continue;
                }
                views.push_back(jstr(s, "availabilityView"));
            }
        const long long step = interval * 60'000LL;
        const long long slots = (to - from) / step;
        const long long need = (duration * 60'000LL + step - 1) / step;
        const bool working_hours = jbool(a, "working_hours_only", true);
        const int max_results = std::clamp(static_cast<int>(jint(a, "max_results", 5)), 1, 20);
        const auto free_at = [&](long long i) {
            for (const auto& v : views)
                if (i >= static_cast<long long>(v.size()) || v[static_cast<size_t>(i)] != '0') return false;
            if (working_hours) {
                const std::time_t t = static_cast<std::time_t>((from + i * step) / 1000);
                std::tm local{};
                localtime_s(&local, &t);
                if (local.tm_wday == 0 || local.tm_wday == 6) return false;
                const int minute = local.tm_hour * 60 + local.tm_min;
                if (minute < 8 * 60 || minute + interval > 17 * 60) return false;
            }
            return true;
        };
        json found = json::array();
        for (long long i = 0; i + need <= slots && static_cast<int>(found.size()) < max_results;) {
            long long k = 0;
            while (k < need && free_at(i + k)) ++k;
            if (k == need) {
                const long long s = from + i * step;
                found.push_back({{"start", local_iso(s)}, {"end", local_iso(s + duration * 60'000LL)}});
                i += need;
            } else {
                i += k + 1;
            }
        }
        json out = {{"slots", found}, {"duration_minutes", duration}, {"attendees", schedules}};
        if (!unknown.empty()) out["free_busy_unknown"] = unknown;
        return ToolOutput::success(out.dump());
    }

    ToolOutput create_event(ToolContext& ctx, const json& a) {
        auto c = conn();
        if (!c) return fail(c.error());
        const std::string id = jstr(a, "id");
        json body = json::object();
        if (a.contains("subject")) body["subject"] = jstr(a, "subject");
        if (a.contains("body")) body["body"] = {{"contentType", "text"}, {"content", jstr(a, "body")}};
        if (a.contains("location")) body["location"] = {{"displayName", jstr(a, "location")}};
        const auto start = time_arg(a, "start"), end = time_arg(a, "end");
        if (a.contains("start") && !start) return ToolOutput::failure("start is not an ISO 8601 date-time");
        if (a.contains("end") && !end) return ToolOutput::failure("end is not an ISO 8601 date-time");
        if (start) body["start"] = {{"dateTime", iso8601_graph(*start)}, {"timeZone", "UTC"}};
        if (end) body["end"] = {{"dateTime", iso8601_graph(*end)}, {"timeZone", "UTC"}};
        if (a.contains("attendees") || a.contains("optional_attendees")) {
            json att = json::array();
            for (const auto& x : string_list(a, "attendees")) att.push_back({{"emailAddress", {{"address", x}}}, {"type", "required"}});
            for (const auto& x : string_list(a, "optional_attendees")) att.push_back({{"emailAddress", {{"address", x}}}, {"type", "optional"}});
            body["attendees"] = att;
        }
        Result<json> r = json(nullptr);
        if (id.empty()) {
            if (!start || !end || *end <= *start) return ToolOutput::failure("start and end are required, and end must be after start");
            // The call id makes a retried create idempotent (Graph transactionId).
            std::string tx;
            for (const char ch : ctx.call_id.empty() ? random_hex_id() : ctx.call_id)
                if (std::isalnum(static_cast<unsigned char>(ch))) tx += ch;
            body["transactionId"] = tx;
            auto g = call(ctx, "/me/events", "POST");
            g.body = body;
            r = (*c)->graph->call(g);
        } else {
            auto g = call(ctx, "/me/events/" + url_encode(id), "PATCH");
            g.body = body;
            r = (*c)->graph->call(g);
        }
        if (!r) return fail(r.error());
        json out = {{"id", jstr(*r, "id")}, {"created", id.empty()}, {"invitations_sent", !string_list(a, "attendees").empty() || !string_list(a, "optional_attendees").empty()}};
        if (auto s = shown_event(*r, svc_.rules(), false)) out["event"] = *s;
        return ToolOutput::success(out.dump());
    }

    static std::string random_hex_id() { return std::to_string(unix_millis_now()); }

    ToolOutput respond(ToolContext& ctx, const json& a) {
        auto c = conn();
        if (!c) return fail(c.error());
        const std::string id = jstr(a, "id"), response = jstr(a, "response");
        std::string action;
        if (response == "accept") action = "accept";
        else if (response == "tentative") action = "tentativelyAccept";
        else if (response == "decline") action = "decline";
        else return ToolOutput::failure("response must be accept, tentative or decline");
        if (id.empty()) return ToolOutput::failure("id is required");
        auto g = call(ctx, "/me/events/" + url_encode(id) + "/" + action, "POST");
        g.body = json{{"comment", jstr(a, "comment")}, {"sendResponse", true}};
        if (auto r = (*c)->graph->call(g); !r) return fail(r.error());
        return ToolOutput::success(json{{"responded", response}, {"id", id}}.dump());
    }

private:
    MailService& svc_;
};

}  // namespace

std::vector<ToolDef> MailService::tools() {
    auto t = std::make_shared<Tools>(*this);
    std::vector<ToolDef> out;
    const auto add = [&](const char* id, const char* description, Risk risk, json params, auto fn, const char* source = "mail") {
        ToolDef d;
        d.id = id;
        d.description = description;
        d.risk = risk;
        d.parameters = std::move(params);
        d.offered = [this] { return available(); };
        d.run = [t, fn](ToolContext& ctx, const json& a) { return fn(*t, ctx, a); };
        d.provenance = {source, "", "", false};
        out.push_back(std::move(d));
    };

    add("mail.search",
        "Search the user's mail by words, folder, sender, date range or attachments. Returns message ids, subjects, senders and "
        "previews, newest first. Messages withheld because of their sensitivity label are listed without content.",
        Risk::read,
        schema({{"query", {{"type", "string"}, {"description", "Words to search for in subject, body, people and attachment names"}}},
                {"folder", {{"type", "string"}, {"description", "Folder name or well-known name (inbox, sentitems, drafts, archive)"}}},
                {"from", {{"type", "string"}, {"description", "Part of the sender's name or address"}}},
                {"since", kTime},
                {"until", kTime},
                {"has_attachments", {{"type", "boolean"}}},
                {"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 50}}}}),
        [](Tools& x, ToolContext& c, const json& a) { return x.search(c, a); });
    add("mail.read",
        "Read a message with its text body and attachment list; with thread=true, the whole conversation; with attachment_id, the "
        "text of that attachment (pdf, docx, xlsx, pptx or text).",
        Risk::read,
        schema({{"id", {{"type", "string"}}}, {"thread", {{"type", "boolean"}}}, {"attachment_id", {{"type", "string"}}}}, {"id"}),
        [](Tools& x, ToolContext& c, const json& a) { return x.read(c, a); });
    add("mail.draft",
        "Create or update a draft in the user's Drafts folder: a new message, a reply (reply_to, optionally reply_all), a forward "
        "(forward_of), or changes to an existing draft (draft_id). Nothing is sent.",
        Risk::external,
        schema({{"draft_id", {{"type", "string"}}},
                {"reply_to", {{"type", "string"}, {"description", "Message id to reply to"}}},
                {"reply_all", {{"type", "boolean"}}},
                {"forward_of", {{"type", "string"}, {"description", "Message id to forward"}}},
                {"to", kAddresses},
                {"cc", kAddresses},
                {"subject", {{"type", "string"}}},
                {"body", {{"type", "string"}, {"description", "Plain text"}}}}),
        [](Tools& x, ToolContext& c, const json& a) { return x.draft(c, a); });
    add("mail.send",
        "Send an email. The user always sees and approves the exact recipients, subject and body first. reply_to keeps the "
        "message in the conversation of the message replied to.",
        Risk::external,
        schema({{"to", kAddresses},
                {"cc", kAddresses},
                {"subject", {{"type", "string"}}},
                {"body", {{"type", "string"}, {"description", "Plain text, exactly as it will be sent"}}},
                {"reply_to", {{"type", "string"}, {"description", "Message id this replies to"}}}},
               {"to", "subject", "body"}),
        [](Tools& x, ToolContext& c, const json& a) { return x.send(c, a); });
    add("mail.move", "Move a message to another folder.", Risk::destructive,
        schema({{"id", {{"type", "string"}}}, {"folder", {{"type", "string"}, {"description", "Folder name or well-known name"}}}}, {"id", "folder"}),
        [](Tools& x, ToolContext& c, const json& a) { return x.move(c, jstr(a, "id"), jstr(a, "folder")); });
    add("mail.delete", "Delete a message: it moves to Deleted Items and can be restored from there.", Risk::destructive,
        schema({{"id", {{"type", "string"}}}}, {"id"}),
        [](Tools& x, ToolContext& c, const json& a) { return x.move(c, jstr(a, "id"), "deleteditems"); });

    add("calendar.list", "List the user's calendar events in a time range (default: the next 7 days). Private events show only their time.",
        Risk::read, schema({{"start", kTime}, {"end", kTime}, {"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 200}}}}),
        [](Tools& x, ToolContext& c, const json& a) { return x.list_events(c, a); });
    add("calendar.get", "Read one calendar event with its description and attendees.", Risk::read, schema({{"id", {{"type", "string"}}}}, {"id"}),
        [](Tools& x, ToolContext& c, const json& a) { return x.get_event(c, a); });
    add("calendar.find_free_time",
        "Find times when the user and the attendees are all free, for a meeting of duration_minutes within a range (default: the "
        "next 7 days, working hours only). Times are in the user's time zone.",
        Risk::read,
        schema({{"attendees", kAddresses},
                {"duration_minutes", {{"type", "integer"}, {"minimum", 5}, {"maximum", 1440}}},
                {"start", kTime},
                {"end", kTime},
                {"working_hours_only", {{"type", "boolean"}}},
                {"max_results", {{"type", "integer"}, {"minimum", 1}, {"maximum", 20}}}},
               {"duration_minutes"}),
        [](Tools& x, ToolContext& c, const json& a) { return x.free_time(c, a); });
    add("calendar.create",
        "Create a calendar event, or update one (id). Attendees receive invitations.", Risk::external,
        schema({{"id", {{"type", "string"}, {"description", "Event id to update; omit to create"}}},
                {"subject", {{"type", "string"}}},
                {"start", kTime},
                {"end", kTime},
                {"attendees", kAddresses},
                {"optional_attendees", kAddresses},
                {"location", {{"type", "string"}}},
                {"body", {{"type", "string"}}}}),
        [](Tools& x, ToolContext& c, const json& a) { return x.create_event(c, a); });
    add("calendar.respond", "Accept, tentatively accept or decline a meeting invitation, with an optional message to the organiser.",
        Risk::external,
        schema({{"id", {{"type", "string"}}},
                {"response", {{"type", "string"}, {"enum", {"accept", "tentative", "decline"}}}},
                {"comment", {{"type", "string"}}}},
               {"id", "response"}),
        [](Tools& x, ToolContext& c, const json& a) { return x.respond(c, a); });
    return out;
}

}  // namespace wb::mail
