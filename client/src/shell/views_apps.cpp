// The Apps view (client/local-apps Apps View): the user's Local apps with name, description, size and last use, and
// Open, Stop, Show folder, Change with the agent and Delete. Opening an app whose MCP tools are not confirmed asks the
// user first. The host does the work (apps.list, .open, .confirm, .stop, .reveal, .delete); a change request is a new
// Thread sent through inbox.send.
#include "shell/feed.h"
#include "shell/format.h"
#include "shell/shell.h"
#include "ui/text16.h"

namespace wb::shell {

using ui::Node;
using ui::Paint;
using ui::TextStyle;

namespace {

std::u16string u16(const std::string& s) { return ui::to_utf16(s); }

std::u16string join(const json& list) {
    std::u16string s;
    if (list.is_array())
        for (const auto& x : list)
            if (x.is_string()) s += (s.empty() ? u"" : u", ") + u16(x.get<std::string>());
    return s;
}

}  // namespace

void PagePane::load_apps() {
    std::weak_ptr<int> alive = alive_;
    shell_.call("apps.list", json::object(), [this, alive](Result<json> res) {
        if (!alive.lock()) return;
        if (res) {
            apps_ = *res;
            errors_.erase("apps");
        } else {
            errors_["apps"] = u16(res.error().message);
        }
        rebuild();
    });
}

void PagePane::apps_changed() {
    if (shell_.route().name == RouteName::apps) load_apps();
}

void PagePane::open_app(const std::string& id, const std::u16string& title) {
    act("apps", "apps.open", {{"id", id}}, [this, id, title](const json& r) {
        if (!jbool(r, "needs_confirmation")) {
            shell_.announce(shell_.t("apps.opened", {{"name", title}}));
            load_apps();
            return;
        }
        // MCP Bridge: the user confirms the app's MCP tools before its pages can call them.
        auto d = std::make_unique<ui::Dialog>(shell_.ui(), "apps-confirm-dialog", shell_.t("apps.confirm_title"));
        d->body().emplace<ui::Label>(shell_.ui(), shell_.t("apps.confirm_body", {{"name", title}, {"tools", join(r.value("mcp_tools", json::array()))}}));
        ui::Dialog* dp = d.get();
        d->add_button("apps-confirm-cancel", shell_.t("settings.cancel"), ui::Button::Kind::standard, [dp] { dp->close(); });
        d->add_button("apps-confirm-allow", shell_.t("apps.allow"), ui::Button::Kind::primary, [this, dp, id, title] {
            dp->close();
            act("apps", "apps.confirm", {{"id", id}}, [this, id, title](const json&) { open_app(id, title); });
        });
        shell_.window().modal().show(std::move(d));
        shell_.window().render();
    });
}

void PagePane::ask_delete_app(const std::string& id, const std::u16string& title) {
    auto d = std::make_unique<ui::Dialog>(shell_.ui(), "apps-delete-dialog", shell_.t("apps.delete_title"));
    d->body().emplace<ui::Label>(shell_.ui(), shell_.t("apps.delete_body", {{"name", title}}));
    ui::Dialog* dp = d.get();
    d->add_button("apps-delete-cancel", shell_.t("settings.cancel"), ui::Button::Kind::standard, [dp] { dp->close(); });
    d->add_button("apps-delete-confirm", shell_.t("apps.delete"), ui::Button::Kind::primary, [this, dp, id, title] {
        dp->close();
        act("apps", "apps.delete", {{"id", id}}, [this, title](const json& r) {
            apps_ = r;
            shell_.announce(shell_.t("apps.deleted", {{"name", title}}));
        });
    });
    shell_.window().modal().show(std::move(d));
    shell_.window().render();
}

void PagePane::ask_change_app(const std::string& id, const std::u16string& title) {
    shell_.ask_text_value("apps-change-dialog", shell_.t("apps.change_title", {{"name", title}}), shell_.t("apps.change_label"), u"",
                          [this, id, title](const std::string& request) {
                              const std::string text = ui::to_utf8(shell_.t("apps.change_message", {{"name", title}, {"id", u16(id)}, {"request", u16(request)}}));
                              std::weak_ptr<int> alive = alive_;
                              shell_.call("inbox.send", {{"thread_key", "new"}, {"text", text}, {"mode", "queue"}}, [this, alive](Result<json> r) {
                                  if (!alive.lock()) return;
                                  if (!r) {
                                      errors_["apps"] = u16(r.error().message);
                                      return rebuild();
                                  }
                                  if (const std::string thread = jstr(*r, "thread_id"); !thread.empty()) shell_.navigate(thread_hash(thread));
                              });
                          });
}

void PagePane::build_apps() {
    text(*this, shell_.t("apps.body"), Paint::text_secondary, "apps-body");
    if (!apps_.is_object()) {
        if (!errors_.contains("apps")) text(*this, shell_.t("common.loading"), Paint::text_secondary);
        error_line(*this, "apps");
        return;
    }
    if (const std::string why = jstr(apps_, "unavailable"); !why.empty())
        text(*this, shell_.t("apps.unavailable", {{"reason", u16(why)}}), Paint::warning, "apps-unavailable");
    const bool can_run = jstr(apps_, "unavailable").empty();
    auto* list = emplace<Node>("apps-list");
    list->role = ui::Role::list;
    list->name = shell_.t("titles.apps");
    list->gap = 8;
    const json* all = jobj(apps_, "apps");
    if (!all || all->empty()) text(*list, shell_.t("apps.empty"), Paint::text_secondary, "apps-empty");
    const char decimal = shell_.decimal();
    if (all)
        for (const auto& a : *all) {
            const std::string id = jstr(a, "id");
            const std::u16string title = u16(jstr(a, "name"));
            const bool running = jbool(a, "running");
            auto* item = list->emplace<Node>("app-" + id);
            item->role = ui::Role::list_item;
            item->gap = 4;
            item->name = title;
            auto* head = item->emplace<Node>();
            head->axis = ui::Axis::row;
            head->gap = 8;
            head->emplace<ui::Label>(shell_.ui(), title, TextStyle::body_strong, Paint::text, true)->role = ui::Role::none;
            if (running) head->emplace<ui::Label>(shell_.ui(), shell_.t("apps.running"), TextStyle::caption, Paint::success, false, "app-running-" + id);
            if (const std::string desc = jstr(a, "description"); !desc.empty()) text(*item, u16(desc), Paint::text_secondary, "app-description-" + id);
            const std::string used = jstr(a, "last_used");
            const std::u16string when = used.empty() ? shell_.t("apps.never") : format_relative(used, shell_.now(), shell_.ui().strings, [](long long ms) {
                return ui::to_utf16(iso_from_millis(ms).substr(0, 10));
            });
            text(*item, shell_.t("apps.meta", {{"size", u16(format_bytes(static_cast<double>(jint(a, "size")), decimal))}, {"when", when}}),
                 Paint::text_secondary, "app-meta-" + id);
            if (const std::string p = jstr(a, "problem"); !p.empty()) text(*item, u16(p), Paint::warning, "app-problem-" + id);
            auto* actions = item->emplace<Node>();
            actions->axis = ui::Axis::row;
            actions->gap = 8;
            auto* open = button(*actions, "app-open-" + id, shell_.t("apps.open"), [this, id, title] { open_app(id, title); });
            open->name = shell_.t("apps.open_named", {{"name", title}});
            open->enabled = busy_.empty() && can_run && jstr(a, "problem").empty();
            if (running) {
                auto* stop = button(*actions, "app-stop-" + id, shell_.t("apps.stop"), [this, id, title] {
                    act("apps", "apps.stop", {{"id", id}}, [this, title](const json& r) {
                        apps_ = r;
                        shell_.announce(shell_.t("apps.stopped", {{"name", title}}));
                    });
                }, ui::Button::Kind::subtle);
                stop->name = shell_.t("apps.stop_named", {{"name", title}});
                stop->enabled = busy_.empty();
            }
            auto* reveal = button(*actions, "app-folder-" + id, shell_.t("apps.folder"),
                                  [this, id] { act("apps", "apps.reveal", {{"id", id}}); }, ui::Button::Kind::subtle);
            reveal->name = shell_.t("apps.folder_named", {{"name", title}});
            reveal->enabled = busy_.empty();
            auto* change = button(*actions, "app-change-" + id, shell_.t("apps.change"), [this, id, title] { ask_change_app(id, title); },
                                  ui::Button::Kind::subtle);
            change->name = shell_.t("apps.change_named", {{"name", title}});
            change->enabled = busy_.empty() && shell_.runs_blocked().empty();
            auto* del = button(*actions, "app-delete-" + id, shell_.t("apps.delete"), [this, id, title] { ask_delete_app(id, title); },
                               ui::Button::Kind::subtle);
            del->name = shell_.t("apps.delete_named", {{"name", title}});
            del->enabled = busy_.empty();
        }
    error_line(*this, "apps");
}

}  // namespace wb::shell
