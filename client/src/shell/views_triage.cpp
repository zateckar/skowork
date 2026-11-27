// Event triage (client/event-triage, improve-proactive-durable-work D1, D2): the Settings section — opt-in, the scope, the
// responsibilities, status, gaps and Delete triage data — and the suggestion card of a suggestion Thread with Start work
// and Dismiss. The host does the work (triage.status, .configure, .ack_gap, .delete, .suggestion, .start, .dismiss).
#include "shell/shell.h"
#include "ui/text16.h"
#include "ui/textfield.h"

namespace wb::shell {

using ui::Node;
using ui::Paint;
using ui::TextStyle;

namespace {

std::u16string u16(const std::string& s) { return ui::to_utf16(s); }


}  // namespace

// ---- Settings > Triage --------------------------------------------------------------------------------------------------

void PagePane::load_triage() {
    std::weak_ptr<int> alive = alive_;
    shell_.call("triage.status", json::object(), [this, alive](Result<json> res) {
        if (!alive.lock()) return;
        if (res) {
            triage_ = *res;
            errors_.erase("triage");
        } else {
            errors_["triage"] = u16(res.error().message);
        }
        rebuild();
    });
}

void PagePane::triage_changed() {
    if (shell_.route().name == RouteName::settings) load_triage();
}

void PagePane::build_triage(Node& c) {
    text(c, shell_.t("settings.triage.body"), Paint::text_secondary);
    if (!triage_.is_object()) {
        if (!errors_.contains("triage")) text(c, shell_.t("common.loading"), Paint::text_secondary);
        error_line(c, "triage");
        return;
    }
    const json& s = triage_;
    const auto configure = [this](json patch, const char* said) {
        act("triage", "triage.configure", std::move(patch), [this, said](const json&) {
            shell_.announce(shell_.t(said));
            load_triage();
        });
    };
    const bool on = jbool(s, "enabled");
    setting_switch(c, "triage-enabled", shell_.t("settings.triage.enabled"), on, false,
                   [configure](bool v) { configure({{"enabled", v}}, "settings.triage.saved"); });
    if (!jbool(s, "available"))
        text(c, shell_.t("settings.triage.unavailable"), Paint::warning, "triage-unavailable");
    if (on) {
        // Scope: mail folders, and the calendar.
        c.emplace<ui::Label>(shell_.ui(), shell_.t("settings.triage.scope"), TextStyle::body_strong)->role = ui::Role::heading;
        std::vector<std::string> folders;
        if (const json* f = jobj(s, "folders"); f && f->is_array())
            for (const auto& x : *f)
                if (x.is_string()) folders.push_back(x.get<std::string>());
        auto* list = c.emplace<Node>("triage-folders");
        list->role = ui::Role::list;
        list->name = shell_.t("settings.triage.folders");
        list->gap = 4;
        for (const auto& folder : folders) {
            auto* item = list->emplace<Node>("triage-folder-" + folder);
            item->role = ui::Role::list_item;
            item->axis = ui::Axis::row;
            item->gap = 8;
            const std::u16string what = shell_.t("settings.triage.folder", {{"name", u16(folder)}});
            item->name = what;
            {
                auto* l = item->emplace<ui::Label>(shell_.ui(), what, TextStyle::body, Paint::text, true);
                l->role = ui::Role::none;
                l->padding.top = 6;  // fills the row, so the action sits at its end (Settings Row Layout); in line with the button's text
            }
            auto* rm = button(*item, "triage-folder-remove-" + folder, shell_.t("settings.mail.remove"), [configure, folders, folder] {
                json rest = json::array();
                for (const auto& f : folders)
                    if (f != folder) rest.push_back(f);
                configure({{"folders", rest}}, "settings.triage.saved");
            }, ui::Button::Kind::subtle);
            rm->name = shell_.t("settings.mail.remove_named", {{"name", what}});
            rm->enabled = busy_.empty();
        }
        if (folders.empty()) text(*list, shell_.t("settings.triage.no_folders"), Paint::text_secondary, "triage-no-folders");
        auto* field = c.emplace<ui::TextField>(shell_.ui(), "triage-folder-name", shell_.t("settings.triage.folder_name"));
        field->set_text(triage_folder_);
        ui::TextField* fp = field;
        field->on_change = [this, fp] { triage_folder_ = fp->text(); };
        button(c, "triage-folder-add", shell_.t("settings.triage.add_folder"), [this, configure, folders] {
            const std::string value = ui::to_utf8(triage_folder_);
            if (value.find_first_not_of(" \t") == std::string::npos) return;
            json all = json::array();
            for (const auto& f : folders) all.push_back(f);
            all.push_back(value);
            triage_folder_.clear();
            configure({{"folders", all}}, "settings.triage.saved");
        })->enabled = busy_.empty();
        setting_switch(c, "triage-calendar", shell_.t("settings.triage.calendar"), jbool(s, "calendar"), false,
                       [configure](bool v) { configure({{"calendar", v}}, "settings.triage.saved"); });
        setting_switch(c, "triage-teams", shell_.t("settings.triage.teams"), jbool(s, "teams"), false,
                       [configure](bool v) { configure({{"teams", v}}, "settings.triage.saved"); });
        setting_switch(c, "triage-auto-prepare", shell_.t("settings.triage.auto_prepare"), jbool(s, "auto_prepare", true), false,
                       [configure](bool v) { configure({{"auto_prepare", v}}, "settings.triage.saved"); });

        // Responsibilities: what the model classifies against.
        c.emplace<ui::Label>(shell_.ui(), shell_.t("settings.triage.responsibilities"), TextStyle::body_strong)->role = ui::Role::heading;
        text(c, shell_.t("settings.triage.responsibilities_body"), Paint::text_secondary);
        if (!triage_duties_) triage_duties_ = u16(jstr(s, "responsibilities"));
        auto* duties = c.emplace<ui::TextField>(shell_.ui(), "triage-responsibilities", shell_.t("settings.triage.responsibilities"));
        duties->set_text(*triage_duties_);
        ui::TextField* dp = duties;
        duties->on_change = [this, dp] { triage_duties_ = dp->text(); };
        button(c, "triage-responsibilities-save", shell_.t("settings.triage.save"), [this, configure] {
            const std::string value = ui::to_utf8(triage_duties_.value_or(u""));
            triage_duties_.reset();
            configure({{"responsibilities", value}}, "settings.triage.saved");
        })->enabled = busy_.empty();

        // Status.
        c.emplace<ui::Label>(shell_.ui(), shell_.t("settings.triage.status"), TextStyle::body_strong)->role = ui::Role::heading;
        if (jstr(s, "baseline") == "pending") text(c, shell_.t("settings.triage.baseline"), Paint::text_secondary, "triage-baseline");
        if (jbool(s, "paused")) text(c, shell_.t("settings.triage.paused"), Paint::text_secondary, "triage-paused");
        row(c, "triage-counts", shell_.t("settings.triage.counts_label"),
            shell_.t("settings.triage.counts", {{"pending", shell_.number(jint(s, "pending"))}, {"suggestions", shell_.number(jint(s, "suggestions"))},
                                                {"dismissed", shell_.number(jint(s, "dismissed"))}}));
        // Each count agrees with its noun (Plain Settings Wording): "1 item expired before it was read."
        if (const long long expired = jint(s, "expired"), uncertain = jint(s, "uncertain"); expired > 0 || uncertain > 0) {
            std::u16string skipped;
            if (expired > 0) skipped = shell_.count("settings.triage.expired", expired);
            if (uncertain > 0) skipped += (skipped.empty() ? u"" : u" ") + shell_.count("settings.triage.unclassified", uncertain);
            text(c, skipped, Paint::text_secondary, "triage-skipped");
        }
        if (const std::string err = jstr(s, "last_error"); !err.empty())
            text(c, shell_.t("settings.triage.last_error", {{"reason", u16(err)}}), Paint::warning, "triage-last-error");
        // Gaps: a scope paused at the 1,000-event limit until the user acknowledges it.
        if (const json* gaps = jobj(s, "gaps"); gaps && gaps->is_object() && !gaps->empty()) {
            for (const auto& [scope, g] : gaps->items())
                text(c, shell_.t("settings.triage.gap", {{"scope", shell_.t("settings.triage.scope_" + scope)},
                                                          {"since", shell_.moment_of(jstr(g, "since"))},
                                                          {"missed", shell_.number(jint(g, "missed"))},
                                                          {"limit", shell_.number(jint(s, "limit", 1000))}}),
                     Paint::warning, "triage-gap-" + scope);
            button(c, "triage-gap-ack", shell_.t("settings.triage.resume"), [this] {
                act("triage", "triage.ack_gap", json::object(), [this](const json&) {
                    shell_.announce(shell_.t("settings.triage.resumed"));
                    load_triage();
                });
            })->enabled = busy_.empty();
        }
    }
    auto* del = button(c, "triage-delete", shell_.t("settings.triage.delete"), [this] {
        auto d = std::make_unique<ui::Dialog>(shell_.ui(), "triage-delete-dialog", shell_.t("settings.triage.delete_title"));
        d->body().emplace<ui::Label>(shell_.ui(), shell_.t("settings.triage.delete_body"));
        ui::Dialog* dp = d.get();
        d->add_button("triage-delete-cancel", shell_.t("settings.cancel"), ui::Button::Kind::standard, [dp] { dp->close(); });
        d->add_button("triage-delete-confirm", shell_.t("settings.triage.delete"), ui::Button::Kind::primary, [this, dp] {
            dp->close();
            act("triage", "triage.delete", json::object(), [this](const json&) {
                shell_.announce(shell_.t("settings.triage.deleted"));
                load_triage();
            });
        });
        shell_.window().modal().show(std::move(d));
        shell_.window().render();
    }, ui::Button::Kind::subtle);
    del->enabled = busy_.empty();
    error_line(c, "triage");
}

// ---- The suggestion card of a Thread ----------------------------------------------------------------------------------

void ThreadPane::triage_changed() {
    if (!suggestion_thread_.empty()) load_suggestion();
}

void ThreadPane::load_suggestion() {
    std::weak_ptr<int> alive = alive_;
    const std::string id = suggestion_thread_;
    shell_.call("triage.suggestion", {{"thread_id", id}}, [this, alive, id](Result<json> res) {
        if (!alive.lock() || id != suggestion_thread_) return;
        suggestion_data_ = res ? *res : json(nullptr);
        rebuild_suggestion();
    });
}

void ThreadPane::rebuild_suggestion() {
    if (!suggestion_) return;
    suggestion_->clear_children();
    suggestion_->padding = {};
    const json& s = suggestion_data_;
    if (!s.is_object()) {
        suggestion_->relayout();
        return;
    }
    suggestion_->padding = {20, 4, 12, 8};
    suggestion_->gap = 6;
    suggestion_->role = ui::Role::group;
    suggestion_->name = shell_.t("thread.suggestion.title");
    const std::string status = jstr(s, "status");
    if (status == "withdrawn") {
        suggestion_->emplace<ui::Label>(shell_.ui(), shell_.t("thread.suggestion.withdrawn"), TextStyle::body, Paint::text_secondary, true,
                                        "thread-suggestion-withdrawn");
        suggestion_->relayout();
        return;
    }
    const json empty = json::object();
    const json* src = jobj(s, "source");
    if (!src) src = &empty;
    suggestion_->emplace<ui::Label>(shell_.ui(), shell_.t("thread.suggestion.title"), TextStyle::body_strong)->role = ui::Role::heading;
    const std::string scope = jstr(s, "scope");
    suggestion_->emplace<ui::Label>(
        shell_.ui(),
        shell_.t(scope == "calendar" ? "thread.suggestion.event" : scope == "teams" ? "thread.suggestion.teams" : "thread.suggestion.message",
                 {{"subject", u16(jstr(*src, "subject"))}, {"from", u16(jstr(*src, "from"))}, {"at", shell_.moment_of(jstr(*src, "at"))}}),
        TextStyle::body, Paint::text, true, "thread-suggestion-source");
    if (const std::string reason = jstr(s, "reason"); !reason.empty())
        suggestion_->emplace<ui::Label>(shell_.ui(), shell_.t("thread.suggestion.reason", {{"reason", u16(reason)}}), TextStyle::caption,
                                        Paint::text_secondary, true, "thread-suggestion-reason");
    auto* actions = suggestion_->emplace<Node>("thread-suggestion-actions");
    actions->axis = ui::Axis::row;
    actions->gap = 8;
    const std::string thread = suggestion_thread_;
    const auto run = [this, thread](const char* method, const char* said) {
        suggestion_busy_ = true;
        rebuild_suggestion();
        std::weak_ptr<int> alive = alive_;
        shell_.call(method, {{"thread_id", thread}}, [this, alive, said](Result<json> res) {
            if (!alive.lock()) return;
            suggestion_busy_ = false;
            if (res) shell_.announce(shell_.t(said));
            else shell_.announce(shell_.t("thread.suggestion.failed", {{"reason", u16(res.error().message)}}));
            load_suggestion();
        });
    };
    if (status == "open") {
        auto* start = actions->emplace<ui::Button>(shell_.ui(), "thread-suggestion-start", shell_.t("thread.suggestion.start"), ui::Button::Kind::primary);
        start->width = ui::Sizing::fit;
        start->enabled = !suggestion_busy_;
        auto* dismiss = actions->emplace<ui::Button>(shell_.ui(), "thread-suggestion-dismiss", shell_.t("thread.suggestion.dismiss"));
        dismiss->width = ui::Sizing::fit;
        dismiss->enabled = !suggestion_busy_;
        start->on_click = [run] { run("triage.start", "thread.suggestion.started"); };
        dismiss->on_click = [run] { run("triage.dismiss", "thread.suggestion.dismissed"); };
    } else {
        actions->emplace<ui::Label>(shell_.ui(), shell_.t("thread.suggestion.status_" + status), TextStyle::caption, Paint::text_secondary, false,
                                    "thread-suggestion-status");
    }
    if (status == "open" || status == "started") {
        // No action needed (improve-triage-results D7): recorded, the Run cancelled and the Thread archived.
        auto* none = actions->emplace<ui::Button>(shell_.ui(), "thread-suggestion-no-action", shell_.t("thread.suggestion.no_action"));
        none->width = ui::Sizing::fit;
        none->enabled = !suggestion_busy_;
        none->on_click = [run] { run("triage.no_action", "thread.suggestion.no_action_done"); };
    }
    if (const std::string link = jstr(*src, "link"); !link.empty()) {
        auto* open = actions->emplace<ui::Button>(shell_.ui(), "thread-suggestion-open", shell_.t("thread.suggestion.open_source"),
                                                  ui::Button::Kind::subtle);
        open->width = ui::Sizing::fit;
        open->role = ui::Role::link;
        ui::Ui* u = &shell_.ui();
        open->on_click = [u, link] { if (u->open_link) u->open_link(ui::to_utf16(link)); };
    }
    suggestion_->relayout();
}

}  // namespace wb::shell
