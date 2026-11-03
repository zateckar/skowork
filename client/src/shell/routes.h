// Routes of the window (client/work-inbox), the same strings the web UI used as hashes, so tray menu items, toasts
// and the workbench:// handler keep opening the same places:
//   #/inbox, #/inbox/<view>, #/inbox/folder/<id>, #/threads/new, #/threads/<id>[?msg=<run>:<seq>],
//   #/status, #/settings, #/settings/telemetry, #/about,
//   #/skills/new[?thread=<id>] (the Skill editor, with a draft from a Thread), #/skills/edit/<name>,
//   #/automations/<id> (a Trigger and its history), #/automations/new, #/automations/edit/<id>,
//   #/knowledge (the Knowledge view, add-knowledge-sharing design D4), #/apps (the Apps view, add-local-apps),
//   #/chats (the Inbox list of Chats, with the directory in the reading pane), #/chats/<id> (a Chat in the reading pane;
//   add-chats-in-inbox design D3, D4)
#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "shell/inbox_model.h"

namespace wb::shell {

enum class RouteName { inbox, thread, new_thread, status, settings, telemetry, about, skill_editor, automation, automation_editor, knowledge, apps, chat };

// The routes shown in the page pane rather than the Inbox.
inline bool is_page(RouteName n) {
    return n == RouteName::status || n == RouteName::settings || n == RouteName::telemetry || n == RouteName::about ||
           n == RouteName::skill_editor || n == RouteName::automation || n == RouteName::automation_editor ||
           n == RouteName::knowledge || n == RouteName::apps;
}

struct MessageRef {
    std::string run_id;
    long long seq = 0;
    bool operator==(const MessageRef&) const = default;
};

struct Route {
    RouteName name = RouteName::inbox;
    std::string hash = "#/inbox";  // canonical
    ViewId view = "inbox";         // inbox routes
    std::string thread_id;         // thread routes; the Thread a Skill draft comes from; the Chat of a chat route
    std::string skill;             // the private Skill the editor opens (empty: a new one)
    std::string trigger;           // the Trigger shown or edited (empty: a new one)
    std::optional<MessageRef> msg;
};

inline const char* const kSmartViews[] = {"unread", "action", "running", "all", "archive"};

std::string view_hash(const ViewId& view);
std::string thread_hash(const std::string& thread_id, const std::optional<MessageRef>& msg = std::nullopt);
// Accepts "#/about", "/about", "about", with an optional query string or trailing slash; nullopt if unknown.
std::optional<Route> parse_route(std::string_view input);
Route default_route();

}  // namespace wb::shell
