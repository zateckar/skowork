#include "inbox/inbox_service.h"

#include "base/encoding.h"
#include "base/strings.h"
#include "tools/text.h"

#include <algorithm>

namespace wb::inbox {

namespace {

Result<std::string> required(const json& p, const char* key) {
    std::string v = jstr(p, key);
    if (v.empty()) return make_error("invalid_params", std::string(key) + " is required");
    return v;
}

// Host-side texts of the toasts (client/work-inbox Localization); the UI has its own bundles.
struct ToastTexts {
    const char* finished;
    const char* failed;
    const char* approval;
    const char* question;
};

const ToastTexts& texts(const std::string& lang) {
    static const ToastTexts en{"Finished", "Failed", "Approval required", "Question"};
    static const ToastTexts cs{"Dokončeno", "Selhalo", "Vyžaduje schválení", "Otázka"};
    static const ToastTexts de{"Abgeschlossen", "Fehlgeschlagen", "Freigabe erforderlich", "Frage"};
    if (lang == "cs") return cs;
    if (lang == "de") return de;
    return en;
}

// The image type of a stored file by its extension (image.generate names them image-<n>.<ext>); "" when not an image.
std::string image_mime(const std::string& name) {
    const auto dot = name.rfind('.');
    const std::string ext = dot == std::string::npos ? std::string() : to_lower(name.substr(dot + 1));
    if (ext == "png") return "image/png";
    if (ext == "jpg" || ext == "jpeg") return "image/jpeg";
    if (ext == "webp") return "image/webp";
    return {};
}

std::string clip(const std::string& s, size_t n) {
    std::string one = s;
    std::replace(one.begin(), one.end(), '\n', ' ');
    one = trim(one);
    return one.size() > n ? utf8_truncate(one, n - 3) + "..." : one;
}

json run_json(const runtime::RunRecord& r) {
    return {{"id", r.id},
            {"parent_run_id", r.parent_run_id},
            {"state", runtime::to_string(r.state)},
            {"reason", r.reason},
            {"role", r.role},
            {"origin", {{"kind", runtime::to_string(r.origin.kind)}, {"ref", r.origin.ref}}},
            {"unattended", r.unattended},
            {"active_ms", r.active_ms},
            {"model_calls", r.model_calls},  // the totals the Activity log shows per Run (client/model-access)
            {"usage", {{"prompt", r.usage.prompt}, {"completion", r.usage.completion}, {"reasoning", r.usage.reasoning}}},
            {"created_at", r.created_at},
            {"started_at", r.started_at},
            {"ended_at", r.ended_at}};
}

json action_json(const runtime::ActionItem& a) {
    return {{"id", a.id},           {"thread_id", a.thread_id},     {"run_id", a.run_id},
            {"kind", a.kind},       {"call_id", a.call_id},         {"payload", a.payload},
            {"created_at", a.created_at}, {"resolved_at", a.resolved_at}, {"resolution", a.resolution},
            {"raised_by", a.raised_by}};
}

json events_json(const std::vector<runtime::StoredEvent>& events) {
    json out = json::array();
    for (const auto& e : events) {
        // Internal bookkeeping stays in the host.
        if (e.kind == "nonce" || e.kind == "tools_loaded" || e.kind == "subagent_spec") continue;
        out.push_back({{"seq", e.seq}, {"at", e.at}, {"kind", e.kind}, {"payload", e.payload}});
    }
    return out;
}

bool is_setting_key(const std::string& key) {
    if (key == "ui.language" || key == "ui.show_work_details" || key == "dictation.language") return true;
    for (const char* k : kNotificationKinds)
        if (key == std::string("notifications.") + k) return true;
    return false;
}

bool dictation_language(const std::string& v) { return v == "auto" || v == "cs" || v == "de" || v == "en"; }

}  // namespace

std::string title_from(const std::string& text) {
    std::string line = trim(text.substr(0, text.find('\n')));
    if (line.size() > 80) line = utf8_truncate(line, 77) + "...";
    return line;
}

json to_json(const ThreadEntry& e) {
    return {{"id", e.id},
            {"title", e.title},
            {"preview", e.preview},
            {"last_activity", e.last_activity},
            {"created_at", e.created_at},
            {"origin", {{"kind", e.origin_kind}, {"ref", e.origin_ref}}},
            {"unread", e.unread},
            {"archived", e.archived},
            {"pinned", e.pinned},
            {"folder_id", e.folder_id},
            {"has_action", e.has_action},
            {"running", e.running},
            {"open_actions", e.open_actions},
            {"last_run_state", e.last_run_state},
            {"kind", e.kind},
            {"unread_count", e.unread_count},
            {"chat_kind", e.chat_kind},
            {"presence", e.presence}};
}

json to_json(const Counts& c) { return {{"unread", c.unread}, {"action", c.action}, {"running", c.running}}; }

InboxService::InboxService(Database& db, InboxEnv env)
    : env_(std::move(env)), store_(db, env_.clock ? *env_.clock : system_clock()) {
    store_.set_chats_listed(env_.chats_listed);
    if (env_.runtime)
        env_.runtime->set_standalone_handler([this](const runtime::ActionItem& item, const std::string& answer) {
            AnswerHandler h;
            {
                const std::lock_guard lock(cap_mx_);
                if (const auto it = capabilities_.find(item.raised_by); it != capabilities_.end()) h = it->second;
            }
            if (h) h(item, answer);
        });
    thread_ = std::thread([this] { worker(); });
}

InboxService::~InboxService() {
    if (env_.runtime) env_.runtime->set_standalone_handler(nullptr);
    {
        const std::lock_guard lock(q_mx_);
        stopping_ = true;
    }
    q_cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

// ---- Run events ------------------------------------------------------------------------------------------------------

void InboxService::on_event(const json& event) {
    {
        const std::lock_guard lock(q_mx_);
        if (stopping_) return;
        queue_.push_back(event);
    }
    q_cv_.notify_all();
}

void InboxService::wait_idle() {
    std::unique_lock lock(q_mx_);
    q_cv_.wait(lock, [this] { return (queue_.empty() && !busy_) || stopping_; });
}

void InboxService::worker() {
    for (;;) {
        json e;
        {
            std::unique_lock lock(q_mx_);
            q_cv_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            if (stopping_) return;
            e = std::move(queue_.front());
            queue_.pop_front();
            busy_ = true;
        }
        process(e);
        {
            const std::lock_guard lock(q_mx_);
            busy_ = false;
        }
        q_cv_.notify_all();
    }
}

void InboxService::process(const json& e) {
    const std::string kind = jstr(e, "kind");
    const std::string thread = jstr(e, "thread_id");
    const std::string run_id = jstr(e, "run_id");
    const json data = e.contains("data") ? e["data"] : json::object();
    if (thread.empty()) return;
    if (kind == "run.state") {
        const std::string state = jstr(data, "state");
        if ((state == "succeeded" || state == "failed") && env_.runtime) {
            const auto r = env_.runtime->run(run_id);
            if (r && r->parent_run_id.empty()) {
                // Read State: a result that arrives while the Thread is not in focus makes it unread.
                if (!is_focused(thread)) (void)store_.set_unread(thread, true);
                // Occurrence Runs are announced by the scheduler (client/scheduler Run History, design D9).
                if (r->origin.kind != runtime::OriginKind::trigger_occurrence)
                    notify(state == "succeeded" ? "completed" : "failed", thread, data);
            }
        }
        changed(thread);
    } else if (kind == "action.created") {
        notify("action", thread, data);
        changed(thread);
    } else if (kind == "message" || kind == "action.resolved" || kind == "compaction" || kind == "subagent") {
        changed(thread);
    }
}

void InboxService::changed(const std::string& thread_id) {
    if (!env_.emit) return;
    json j = {{"thread_id", thread_id}, {"counts", to_json(store_.counts())}};
    if (const auto e = store_.entry(thread_id)) j["entry"] = to_json(*e);
    env_.emit("inbox.changed", j);
}

void InboxService::chat_changed(const std::string& chat_id) {
    if (!env_.emit) return;
    // No Chat named (the list changed, or messaging was switched on or off): the views load again.
    if (chat_id.empty()) {
        env_.emit("inbox.changed", {{"thread_id", ""}, {"counts", to_json(store_.counts())}});
        return;
    }
    const std::string id = std::string(kChatEntryPrefix) + chat_id;
    json j = {{"thread_id", id}, {"counts", to_json(store_.counts())}};
    if (const auto e = store_.entry(id)) j["entry"] = to_json(*e);
    else j["deleted"] = true;  // left, or no longer listed
    env_.emit("inbox.changed", j);
}

void InboxService::notify_outcome(const std::string& kind, const std::string& thread_id, const std::string& reason) {
    notify(kind, thread_id, {{"reason", reason}});
}

void InboxService::notify(const std::string& kind, const std::string& thread_id, const json& data) {
    if (!env_.notify || !notification_enabled(kind) || is_focused(thread_id)) return;
    const auto entry = store_.entry(thread_id);
    const ToastTexts& t = texts(env_.language ? env_.language() : "en");
    std::string title = entry && !entry->title.empty() ? entry->title : "SkoWork";
    std::string body;
    if (kind == "completed") {
        body = t.finished;
        if (entry && !entry->preview.empty()) body += ": " + clip(entry->preview, 120);
    } else if (kind == "failed") {
        body = std::string(t.failed) + ": " + clip(jstr(data, "reason"), 120);
    } else {
        const json payload = data.contains("payload") ? data["payload"] : json::object();
        body = jstr(data, "kind") == "approval" ? std::string(t.approval) + ": " + jstr(payload, "tool")
                                                : std::string(t.question) + ": " + clip(jstr(payload, "prompt"), 120);
    }
    env_.notify(title, body, thread_id);
}

// ---- Focus -------------------------------------------------------------------------------------------------------------

void InboxService::set_focus(const std::string& thread_id, bool window_focused) {
    const std::lock_guard lock(focus_mx_);
    focus_thread_ = thread_id;
    window_focused_ = window_focused;
}

std::string InboxService::focused_thread() {
    const std::lock_guard lock(focus_mx_);
    return window_focused_ ? focus_thread_ : std::string{};
}

std::string InboxService::open_thread() {
    const std::lock_guard lock(focus_mx_);
    return focus_thread_;
}

bool InboxService::is_focused(const std::string& thread_id) {
    const std::lock_guard lock(focus_mx_);
    return window_focused_ && !thread_id.empty() && focus_thread_ == thread_id;
}

// ---- Standalone Action items ---------------------------------------------------------------------------------------------

void InboxService::register_capability(const std::string& capability, AnswerHandler handler) {
    const std::lock_guard lock(cap_mx_);
    capabilities_[capability] = std::move(handler);
}

Result<runtime::ActionItem> InboxService::raise(const std::string& capability, const std::string& prompt,
                                                const std::vector<std::string>& choices, const std::string& thread_id,
                                                const std::string& copy) {
    if (!env_.runtime) return make_error("unavailable", "the agent runtime is not running");
    std::string thread = thread_id;
    if (thread.empty()) {
        auto t = env_.runtime->create_thread(title_from(prompt));
        if (!t) return t.error();
        thread = *t;
        if (Status s = store_.set_origin(thread, "system", json{{"capability", capability}}); !s) return s.error();
    }
    return env_.runtime->raise_standalone_question(thread, capability, prompt, choices, copy);
}

// ---- Composer ------------------------------------------------------------------------------------------------------------

Result<InboxService::SendOutcome> InboxService::send(const std::string& key, const std::string& text, runtime::MessageMode mode) {
    if (!env_.runtime) return make_error("unavailable", "the agent runtime is not running");
    const Draft draft = store_.draft(key);
    if (trim(text).empty() && draft.attachments.empty()) return make_error("invalid_params", "the message is empty");
    const bool new_thread = key == "new";
    if (!new_thread && !store_.entry(key)) return make_error("not_found", "no such Thread");

    // Attachments become text first, so nothing is created when one cannot be used (design D12).
    struct Converted {
        std::string name, text;
    };
    std::vector<Converted> converted;
    for (const auto& a : draft.attachments) {
        auto bytes = store_.draft_attachment_content(a.id);
        if (!bytes) return bytes.error();
        const std::string doc = tools::document_kind(a.name);
        if (!doc.empty()) {
            if (!env_.extract) return make_error("unsupported_attachment", a.name + ": text extraction is not available");
            runtime::ToolContext ctx;
            ctx.call_id = "attachment";
            auto t = env_.extract(doc, *bytes, ctx);
            if (!t) return make_error("unsupported_attachment", a.name + ": " + t.error().message);
            converted.push_back({a.name, std::move(*t)});
            continue;
        }
        auto decoded = tools::decode_text(*bytes);
        if (!decoded) return make_error("unsupported_attachment", a.name + " is not a text file or a supported document");
        converted.push_back({a.name, std::move(decoded->text)});
    }

    std::string thread = key;
    if (new_thread) {
        auto t = env_.runtime->create_thread(title_from(trim(text).empty() ? converted.front().name : text));
        if (!t) return t.error();
        thread = *t;
    } else if (env_.runtime->runs(thread).empty() && !trim(text).empty()) {
        (void)env_.runtime->store().set_thread_title(thread, title_from(text));
    }
    std::string message = text;
    if (!converted.empty()) {
        message += std::string(message.empty() ? "" : "\n\n") + "Attached files (read them with read_attachment):";
        const json prov_base = {{"source_kind", "files"}, {"trusted", false}};
        for (const auto& c : converted) {
            json prov = prov_base;
            prov["source_id"] = c.name;
            prov["at"] = iso8601_utc(std::chrono::system_clock::now());
            auto id = env_.runtime->store().add_attachment(thread, "", "", c.name, c.text, prov);
            if (!id) {
                if (new_thread) (void)store_.delete_thread(thread);
                return id.error();
            }
            message += "\n- " + c.name + ": attachment " + *id + ", " + std::to_string(c.text.size()) + " bytes of text";
        }
    }
    auto r = env_.runtime->send_message(thread, message, mode);
    if (!r) {
        if (new_thread) (void)store_.delete_thread(thread);  // no empty Thread is left behind; the draft stays
        return r.error();
    }
    (void)store_.clear_draft(key);
    SendOutcome out;
    out.thread_id = thread;
    out.run_id = r->run_id;
    out.queued_message = r->queued_message;
    changed(thread);
    return out;
}

// ---- Settings ------------------------------------------------------------------------------------------------------------

json InboxService::settings() {
    json out = json::object();
    const auto policy = [this](const std::string& key) -> std::optional<json> {
        return env_.policy_value ? env_.policy_value(key) : std::nullopt;
    };
    for (const char* kind : kNotificationKinds) {
        const std::string key = std::string("notifications.") + kind;
        if (const auto p = policy(key); p && p->is_boolean()) {
            out[key] = {{"value", p->get<bool>()}, {"managed", true}};
            continue;
        }
        const auto v = store_.setting(key);
        out[key] = {{"value", !v || *v != "false"}, {"managed", false}};
    }
    out["ui.language"] = {{"value", store_.setting("ui.language").value_or("system")}, {"managed", false}};
    out["ui.show_work_details"] = {{"value", store_.setting("ui.show_work_details").value_or("false") == "true"}, {"managed", false}};
    const std::string dictation = store_.setting("dictation.language").value_or("auto");
    out["dictation.language"] = {{"value", dictation_language(dictation) ? dictation : "auto"}, {"managed", false}};
    return out;
}

Status InboxService::set_setting(const std::string& key, const json& value) {
    if (!is_setting_key(key)) return make_error("invalid_params", "unknown setting " + key);
    const json current = settings();
    if (current[key].value("managed", false)) return make_error("managed", "this setting is managed by your organization");
    if (key == "ui.language") {
        const std::string v = value.is_string() ? value.get<std::string>() : std::string();
        if (v != "system" && v != "cs" && v != "de" && v != "en") return make_error("invalid_params", "language must be system, cs, de or en");
        return store_.set_setting(key, v);
    }
    if (key == "dictation.language") {  // add-dictation D5: Automatic or a transcript language
        const std::string v = value.is_string() ? value.get<std::string>() : std::string();
        if (!dictation_language(v))
            return make_error("invalid_params", "the dictation language must be auto, cs, de or en");
        return store_.set_setting(key, v);
    }
    if (!value.is_boolean()) return make_error("invalid_params", key + " must be true or false");
    return store_.set_setting(key, value.get<bool>() ? "true" : "false");
}

bool InboxService::notification_enabled(const std::string& kind) {
    const json s = settings();
    const std::string key = "notifications." + kind;
    return s.contains(key) && s[key].value("value", true);
}

// ---- API -----------------------------------------------------------------------------------------------------------------

void InboxService::register_rpc(RpcRegistry& rpc, bool expose_on_pipe) {
    const RpcExposure ex = expose_on_pipe ? RpcExposure::ui_and_pipe : RpcExposure::ui;
    InboxStore& st = store_;

    rpc.add("inbox.list", ex, [&st](const json& p) -> Result<json> {
        auto page = st.list(jstr(p, "view", "all"), jstr(p, "cursor"), static_cast<int>(jint(p, "limit", InboxStore::kPageSize)));
        if (!page) return page.error();
        json entries = json::array();
        for (const auto& e : page->entries) entries.push_back(to_json(e));
        return json{{"entries", entries}, {"next_cursor", page->next_cursor}, {"counts", to_json(st.counts())}};
    });
    rpc.add("inbox.counts", ex, [&st](const json&) -> Result<json> { return to_json(st.counts()); });
    rpc.add("inbox.entry", ex, [&st](const json& p) -> Result<json> {
        const auto e = st.entry(jstr(p, "thread_id"));
        if (!e) return make_error("not_found", "no such Thread");
        return to_json(*e);
    });
    rpc.add("inbox.focus", ex, [this](const json& p) -> Result<json> {
        set_focus(jstr(p, "thread_id"), jbool(p, "focused"));
        return json{{"ok", true}};
    });
    rpc.add("threads.open", ex, [this, &st](const json& p) -> Result<json> {
        auto id = required(p, "thread_id");
        if (!id) return id.error();
        if (is_chat_entry(*id)) return make_error("invalid_params", "a Chat opens with messaging.messages");
        if (Status s = st.set_unread(*id, false); !s) return s.error();
        changed(*id);
        return to_json(*st.entry(*id));
    });
    rpc.add("threads.mark_unread", ex, [this, &st](const json& p) -> Result<json> {
        auto id = required(p, "thread_id");
        if (!id) return id.error();
        if (Status s = st.set_unread(*id, jbool(p, "unread", true)); !s) return s.error();
        changed(*id);
        return json{{"ok", true}};
    });
    rpc.add("threads.archive", ex, [this, &st](const json& p) -> Result<json> {
        auto id = required(p, "thread_id");
        if (!id) return id.error();
        if (Status s = st.set_archived(*id, jbool(p, "archived", true)); !s) return s.error();
        changed(*id);
        return json{{"ok", true}};
    });
    rpc.add("threads.pin", ex, [this, &st](const json& p) -> Result<json> {
        auto id = required(p, "thread_id");
        if (!id) return id.error();
        if (Status s = st.set_pinned(*id, jbool(p, "pinned", true)); !s) return s.error();
        changed(*id);
        return json{{"ok", true}};
    });
    rpc.add("threads.rename", ex, [this, &st](const json& p) -> Result<json> {
        auto id = required(p, "thread_id");
        if (!id) return id.error();
        if (Status s = st.rename(*id, jstr(p, "title")); !s) return s.error();
        changed(*id);
        return json{{"ok", true}};
    });
    rpc.add("threads.move", ex, [this, &st](const json& p) -> Result<json> {
        auto id = required(p, "thread_id");
        if (!id) return id.error();
        if (Status s = st.move(*id, jstr(p, "folder_id")); !s) return s.error();
        changed(*id);
        return json{{"ok", true}};
    });
    rpc.add("threads.fork", ex, [this, &st](const json& p) -> Result<json> {
        auto id = required(p, "thread_id");
        if (!id) return id.error();
        auto f = st.fork(*id, jstr(p, "run_id"), jint(p, "seq"));
        if (!f) return f.error();
        changed(*f);
        return json{{"thread_id", *f}};
    });
    rpc.add("threads.delete", ex, [this](const json& p) -> Result<json> {
        auto id = required(p, "thread_id");
        if (!id) return id.error();
        if (is_chat_entry(*id)) return make_error("invalid_params", "a Chat is not deleted; leave it instead");
        if (!jbool(p, "confirm")) return make_error("confirmation_required", "confirm must be true");
        const Status s = env_.forget ? env_.forget("thread", *id) : delete_thread(*id);
        if (!s) return s.error();
        if (env_.emit) env_.emit("inbox.changed", {{"thread_id", *id}, {"deleted", true}, {"counts", to_json(store_.counts())}});
        return json{{"ok", true}};
    });
    // Everything the Thread feed needs in one call (design D13): the entry, the top-level Runs with their event logs,
    // every Action item of the Thread (open and resolved) and the queued messages. Subagent logs load on demand.
    rpc.add("threads.feed", ex, [this, &st](const json& p) -> Result<json> {
        auto id = required(p, "thread_id");
        if (!id) return id.error();
        const auto entry = st.entry(*id);
        if (!entry) return make_error("not_found", "no such Thread");
        if (!env_.runtime) return make_error("unavailable", "the agent runtime is not running");
        runtime::AgentRuntime& rt = *env_.runtime;
        json runs = json::array(), children = json::array(), actions = json::array(), queued = json::array();
        for (const auto& r : rt.runs(*id)) {
            if (!r.parent_run_id.empty()) {
                children.push_back(run_json(r));
                continue;
            }
            json j = run_json(r);
            j["events"] = events_json(rt.events(r.id));
            runs.push_back(std::move(j));
        }
        for (const auto& a : st.actions(*id)) actions.push_back(action_json(a));
        for (const auto& q : rt.queued_messages(*id))
            queued.push_back({{"id", q.id}, {"text", q.text}, {"mode", q.mode == runtime::MessageMode::steer_now ? "steer_now" : "queue"},
                              {"created_at", q.created_at}});
        return json{{"entry", to_json(*entry)}, {"runs", runs}, {"subagents", children}, {"actions", actions}, {"queued", queued}};
    });
    rpc.add("runs.transcript", ex, [this](const json& p) -> Result<json> {
        auto id = required(p, "run_id");
        if (!id) return id.error();
        if (!env_.runtime) return make_error("unavailable", "the agent runtime is not running");
        const auto r = env_.runtime->run(*id);
        if (!r) return make_error("not_found", "no such Run");
        json j = run_json(*r);
        j["events"] = events_json(env_.runtime->events(*id));
        return j;
    });
    // Retry (Error Banners With Remedy): the last top-level Run's request is sent again as a new Run.
    rpc.add("threads.retry", ex, [this](const json& p) -> Result<json> {
        auto id = required(p, "thread_id");
        if (!id) return id.error();
        if (!env_.runtime) return make_error("unavailable", "the agent runtime is not running");
        std::string text;
        for (const auto& r : env_.runtime->runs(*id)) {
            if (!r.parent_run_id.empty() || r.role != "main") continue;
            for (const auto& e : env_.runtime->events(r.id))
                if (e.kind == "message" && jstr(e.payload, "role") == "user") {
                    text = jstr(e.payload, "content");
                    break;
                }
        }
        if (text.empty()) return make_error("nothing_to_retry", "the Thread has no request to retry");
        auto r = env_.runtime->send_message(*id, text, runtime::MessageMode::queue);
        if (!r) return r.error();
        json out = {{"thread_id", *id}};
        if (!r->run_id.empty()) out["run_id"] = r->run_id;
        if (r->queued_message) out["queued_message"] = r->queued_message;
        return out;
    });
    // An image a tool stored in the Thread (add-image-generation: Images In The Thread Feed), for the feed to show and
    // save. Only images, and only from the Thread named.
    rpc.add("threads.attachment", ex, [this](const json& p) -> Result<json> {
        auto thread = required(p, "thread_id");
        if (!thread) return thread.error();
        auto id = required(p, "id");
        if (!id) return id.error();
        if (!env_.runtime) return make_error("unavailable", "the agent runtime is not running");
        const auto info = env_.runtime->store().attachment(*id);
        if (!info || info->thread_id != *thread) return make_error("not_found", "no such attachment in this Thread");
        const std::string mime = image_mime(info->name);
        if (mime.empty()) return make_error("not_an_image", "the attachment is not an image");
        auto bytes = env_.runtime->store().read_attachment(*id, 0, info->size);
        if (!bytes) return bytes.error();
        return json{{"id", info->id}, {"name", info->name}, {"mime", mime}, {"size", info->size},
                    {"data", base64_encode(reinterpret_cast<const std::uint8_t*>(bytes->data()), bytes->size())}};
    });
    rpc.add("threads.context", ex, [this](const json& p) -> Result<json> {
        auto id = required(p, "thread_id");
        if (!id) return id.error();
        if (!env_.runtime) return make_error("unavailable", "the agent runtime is not running");
        const auto u = env_.runtime->context_usage(*id);
        json last = nullptr;
        if (const auto c = env_.runtime->store().compactions(*id); !c.empty())
            last = {{"kind", c.back().kind}, {"at", c.back().at}, {"messages_compacted", c.back().messages_compacted}};
        return json{{"tokens", u.tokens}, {"window", u.window}, {"compaction_threshold", u.compaction_threshold},
                    {"input_budget", u.input_budget}, {"trigger", u.trigger}, {"target", u.target},
                    {"output_reserve", u.output_reserve}, {"estimated", true}, {"includes_tools", u.includes_tools},
                    {"tool_tokens", u.tool_tokens}, {"tools_snapshot", u.includes_tools ? "last_request" : "unavailable"},
                    {"last_compaction", last}};
    });
    rpc.add("folders.list", ex, [&st](const json&) -> Result<json> {
        json out = json::array();
        for (const auto& f : st.folders()) out.push_back({{"id", f.id}, {"name", f.name}});
        return json{{"folders", out}};
    });
    // Every folder change is announced, so a window that did not make it shows it too.
    rpc.add("folders.create", ex, [this, &st](const json& p) -> Result<json> {
        auto f = st.create_folder(jstr(p, "name"));
        if (!f) return f.error();
        if (env_.emit) env_.emit("inbox.changed", {{"thread_id", ""}, {"counts", to_json(st.counts())}});
        return json{{"id", f->id}, {"name", f->name}};
    });
    rpc.add("folders.rename", ex, [this, &st](const json& p) -> Result<json> {
        if (Status s = st.rename_folder(jstr(p, "id"), jstr(p, "name")); !s) return s.error();
        if (env_.emit) env_.emit("inbox.changed", {{"thread_id", ""}, {"counts", to_json(st.counts())}});
        return json{{"ok", true}};
    });
    rpc.add("folders.delete", ex, [this, &st](const json& p) -> Result<json> {
        if (Status s = st.delete_folder(jstr(p, "id")); !s) return s.error();
        if (env_.emit) env_.emit("inbox.changed", {{"thread_id", ""}, {"counts", to_json(st.counts())}});
        return json{{"ok", true}};
    });
    rpc.add("search.query", ex, [&st](const json& p) -> Result<json> {
        json out = json::array();
        for (const auto& h : st.search(jstr(p, "q"), static_cast<int>(jint(p, "limit", 50))))
            out.push_back({{"thread_id", h.thread_id}, {"title", h.title}, {"run_id", h.run_id}, {"seq", h.seq},
                           {"snippet", h.snippet}, {"archived", h.archived}});
        return json{{"hits", out}};
    });
    rpc.add("drafts.get", ex, [&st](const json& p) -> Result<json> {
        auto key = required(p, "thread_key");
        if (!key) return key.error();
        const Draft d = st.draft(*key);
        json atts = json::array();
        for (const auto& a : d.attachments) atts.push_back({{"id", a.id}, {"name", a.name}, {"size", a.size}});
        return json{{"thread_key", d.thread_key}, {"text", d.text}, {"attachments", atts}};
    });
    rpc.add("drafts.save", ex, [&st](const json& p) -> Result<json> {
        auto key = required(p, "thread_key");
        if (!key) return key.error();
        if (Status s = st.save_draft_text(*key, jstr(p, "text")); !s) return s.error();
        return json{{"ok", true}};
    });
    rpc.add("drafts.attach", ex, [&st](const json& p) -> Result<json> {
        auto key = required(p, "thread_key");
        if (!key) return key.error();
        const std::string data = jstr(p, "data");
        if (static_cast<long long>(data.size()) > InboxStore::kMaxAttachmentBytes / 3 * 4 + 8)
            return make_error("too_large", "an attachment can be at most 25 MB");
        const auto bytes = base64_decode(data);
        if (!bytes) return make_error("invalid_params", "data must be base64");
        auto a = st.add_draft_attachment(*key, jstr(p, "name"), std::string_view(reinterpret_cast<const char*>(bytes->data()), bytes->size()));
        if (!a) return a.error();
        return json{{"id", a->id}, {"name", a->name}, {"size", a->size}};
    });
    rpc.add("drafts.detach", ex, [&st](const json& p) -> Result<json> {
        if (Status s = st.remove_draft_attachment(jstr(p, "thread_key"), jstr(p, "id")); !s) return s.error();
        return json{{"ok", true}};
    });
    rpc.add("inbox.send", ex, [this](const json& p) -> Result<json> {
        const auto mode = jstr(p, "mode") == "steer_now" ? runtime::MessageMode::steer_now : runtime::MessageMode::queue;
        auto r = send(jstr(p, "thread_key", "new"), jstr(p, "text"), mode);
        if (!r) return r.error();
        json out = {{"thread_id", r->thread_id}};
        if (!r->run_id.empty()) out["run_id"] = r->run_id;
        if (r->queued_message) out["queued_message"] = r->queued_message;
        return out;
    });
    // The `/` skill list (Composer; client/skills Explicit Skill Invocation): the usable Skills, [{name, description}].
    rpc.add("skills.list", ex, [this](const json&) -> Result<json> {
        return json{{"skills", env_.skills ? env_.skills() : json::array()}};
    });
    rpc.add("settings.get", ex, [this](const json&) -> Result<json> { return settings(); });
    rpc.add("settings.set", ex, [this](const json& p) -> Result<json> {
        if (Status s = set_setting(jstr(p, "key"), p.contains("value") ? p["value"] : json()); !s) return s.error();
        return settings();
    });
}

Status InboxService::delete_thread(const std::string& thread_id) {
    if (env_.runtime) {
        // A queued or interrupted Run ends with the Thread; a running one must be stopped by the user first.
        if (const auto active = env_.runtime->store().active_run(thread_id)) {
            if (active->state == runtime::RunState::running) return make_error("thread_busy", "a Run of this Thread is running; stop it first");
            (void)env_.runtime->cancel(active->id, "the Thread was deleted");
        }
        for (const auto& a : env_.runtime->pending_actions(thread_id))
            if (a.run_id.empty()) (void)env_.runtime->store().resolve_action(a.id, {{"kind", "cancelled"}, {"reason", "the Thread was deleted"}});
    }
    return store_.delete_thread(thread_id);
}

void InboxService::register_dev_rpc(RpcRegistry& rpc) {
    // A test capability that raises Connect / Not now and records the answers it gets (task 4.3).
    register_capability("dev.test", [this](const runtime::ActionItem& item, const std::string& answer) {
        const std::lock_guard lock(cap_mx_);
        dev_answers_.push_back({{"action_id", item.id}, {"thread_id", item.thread_id}, {"answer", answer}});
    });
    rpc.add("dev.raise_action", RpcExposure::ui_and_pipe, [this](const json& p) -> Result<json> {
        auto a = raise("dev.test", jstr(p, "prompt", "Connect your mailbox"), {"Connect", "Not now"}, jstr(p, "thread_id"));
        if (!a) return a.error();
        return json{{"action_id", a->id}, {"thread_id", a->thread_id}};
    });
    rpc.add("dev.action_answers", RpcExposure::ui_and_pipe, [this](const json&) -> Result<json> {
        const std::lock_guard lock(cap_mx_);
        return json{{"answers", dev_answers_}};
    });
}

}  // namespace wb::inbox
