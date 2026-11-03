#include "shell/routes.h"

#include <cctype>
#include <vector>

namespace wb::shell {

namespace {

bool valid_id(std::string_view s) {
    if (s.empty() || s.size() > 64) return false;
    for (char c : s)
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-') return false;
    return true;
}

std::optional<MessageRef> parse_msg(std::string_view query) {
    size_t pos = 0;
    while (pos <= query.size()) {
        size_t amp = query.find('&', pos);
        if (amp == std::string_view::npos) amp = query.size();
        const std::string_view part = query.substr(pos, amp - pos);
        if (part.starts_with("msg=")) {
            const std::string_view v = part.substr(4);
            const size_t colon = v.rfind(':');
            if (colon == std::string_view::npos) return std::nullopt;
            const std::string_view run = v.substr(0, colon), seq = v.substr(colon + 1);
            if (!valid_id(run) || seq.empty() || seq.size() > 12) return std::nullopt;
            long long n = 0;
            for (char c : seq) {
                if (!std::isdigit(static_cast<unsigned char>(c))) return std::nullopt;
                n = n * 10 + (c - '0');
            }
            return MessageRef{std::string(run), n};
        }
        pos = amp + 1;
    }
    return std::nullopt;
}

Route simple(RouteName name, const char* hash) {
    Route r;
    r.name = name;
    r.hash = hash;
    r.view.clear();
    return r;
}

}  // namespace

Route default_route() { return Route{}; }

std::string view_hash(const ViewId& view) {
    if (view == "inbox") return "#/inbox";
    if (view.starts_with("folder:")) return "#/inbox/folder/" + view.substr(7);
    return "#/inbox/" + view;
}

std::string thread_hash(const std::string& thread_id, const std::optional<MessageRef>& msg) {
    std::string h = "#/threads/" + thread_id;
    if (msg) h += "?msg=" + msg->run_id + ":" + std::to_string(msg->seq);
    return h;
}

std::optional<Route> parse_route(std::string_view input) {
    while (!input.empty() && std::isspace(static_cast<unsigned char>(input.front()))) input.remove_prefix(1);
    while (!input.empty() && std::isspace(static_cast<unsigned char>(input.back()))) input.remove_suffix(1);
    if (input.empty()) return std::nullopt;
    if (input.front() == '#') input.remove_prefix(1);
    const size_t q = input.find('?');
    std::string_view path = input.substr(0, q);
    const std::string_view query = q == std::string_view::npos ? std::string_view{} : input.substr(q + 1);
    while (!path.empty() && path.front() == '/') path.remove_prefix(1);
    while (!path.empty() && path.back() == '/') path.remove_suffix(1);
    std::vector<std::string_view> parts;
    for (size_t pos = 0; pos <= path.size();) {
        size_t slash = path.find('/', pos);
        if (slash == std::string_view::npos) slash = path.size();
        parts.push_back(path.substr(pos, slash - pos));
        pos = slash + 1;
    }
    if (parts[0] == "inbox") {
        if (parts.size() == 1) return default_route();
        Route r;
        if (parts.size() == 2) {
            for (const char* v : kSmartViews)
                if (parts[1] == v) {
                    r.view = v;
                    r.hash = view_hash(r.view);
                    return r;
                }
            return std::nullopt;
        }
        if (parts.size() == 3 && parts[1] == "folder" && valid_id(parts[2])) {
            r.view = "folder:" + std::string(parts[2]);
            r.hash = view_hash(r.view);
            return r;
        }
        return std::nullopt;
    }
    if (parts[0] == "chats") {
        Route r;
        if (parts.size() == 1) {
            // The Chats-only view is gone (fold-chats-into-inbox D1): an old link opens the Inbox.
            r.view = "inbox";
            r.hash = "#/inbox";
            return r;
        }
        if (parts.size() != 2 || !valid_id(parts[1])) return std::nullopt;
        r.name = RouteName::chat;
        r.view.clear();
        r.thread_id = std::string(parts[1]);
        r.hash = "#/chats/" + r.thread_id;
        return r;
    }
    if (parts[0] == "threads" && parts.size() == 2) {
        if (parts[1] == "new") return simple(RouteName::new_thread, "#/threads/new");
        if (!valid_id(parts[1])) return std::nullopt;
        Route r;
        r.name = RouteName::thread;
        r.view.clear();
        r.thread_id = std::string(parts[1]);
        r.msg = parse_msg(query);
        r.hash = thread_hash(r.thread_id, r.msg);
        return r;
    }
    if (parts[0] == "skills") {
        Route r = simple(RouteName::skill_editor, "#/skills/new");
        if (parts.size() == 2 && parts[1] == "new") {
            const std::string_view thread = query.starts_with("thread=") ? query.substr(7) : std::string_view{};
            if (!thread.empty()) {
                if (!valid_id(thread)) return std::nullopt;
                r.thread_id = std::string(thread);
                r.hash += "?thread=" + r.thread_id;
            }
            return r;
        }
        if (parts.size() == 3 && parts[1] == "edit" && valid_id(parts[2])) {
            r.skill = std::string(parts[2]);
            r.hash = "#/skills/edit/" + r.skill;
            return r;
        }
        return std::nullopt;
    }
    if (parts[0] == "automations") {
        if (parts.size() == 2 && parts[1] == "new") return simple(RouteName::automation_editor, "#/automations/new");
        Route r;
        if (parts.size() == 2 && valid_id(parts[1])) {
            r = simple(RouteName::automation, "");
            r.trigger = std::string(parts[1]);
            r.hash = "#/automations/" + r.trigger;
            return r;
        }
        if (parts.size() == 3 && parts[1] == "edit" && valid_id(parts[2])) {
            r = simple(RouteName::automation_editor, "");
            r.trigger = std::string(parts[2]);
            r.hash = "#/automations/edit/" + r.trigger;
            return r;
        }
        return std::nullopt;
    }
    if (parts.size() == 2 && parts[0] == "settings" && parts[1] == "telemetry") return simple(RouteName::telemetry, "#/settings/telemetry");
    if (parts.size() == 1) {
        if (parts[0] == "status") return simple(RouteName::status, "#/status");
        if (parts[0] == "settings") return simple(RouteName::settings, "#/settings");
        if (parts[0] == "about") return simple(RouteName::about, "#/about");
        if (parts[0] == "knowledge") return simple(RouteName::knowledge, "#/knowledge");
        if (parts[0] == "apps") return simple(RouteName::apps, "#/apps");

    }
    return std::nullopt;
}

}  // namespace wb::shell
