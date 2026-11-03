// Automations (client/scheduler Automations Section, Run History): the Settings section with the user's Triggers and
// the Remote task history, a Trigger's page with its history, and the editor. The host does the work
// (automations.list, .get, .save, .delete, .run_now).
#include "shell/shell.h"
#include "ui/text16.h"
#include "ui/textfield.h"

namespace wb::shell {

using ui::Node;
using ui::Paint;
using ui::Sizing;
using ui::TextStyle;

namespace {

std::u16string u16(const std::string& s) { return ui::to_utf16(s); }

std::u16string label_or(Shell& s, const std::string& key, const std::string& raw) {
    return s.ui().strings.has(key) ? s.t(key) : u16(raw);
}

// A one-line summary of a Trigger's conditions.
std::u16string conditions_text(Shell& s, const json& t) {
    const std::string kind = jstr(t, "kind");
    const json c = t.contains("conditions") ? t["conditions"] : json::object();
    if (kind == "schedule") {
        const std::string zone = jstr(c, "time_zone");
        if (c.contains("interval_minutes"))
            return s.t("automation.every", {{"minutes", u16(std::to_string(jint(c, "interval_minutes")))}, {"zone", u16(zone)}});
        return s.t("automation.cron", {{"cron", u16(jstr(c, "cron"))}, {"zone", u16(zone)}});
    }
    if (kind == "file") return s.t("automation.files", {{"pattern", u16(jstr(c, "pattern", "*"))}, {"folder", u16(jstr(c, "folder"))}});
    if (kind == "session") return s.t("automation_editor.events." + jstr(c, "event", "logon"));
    return u16(c.dump());
}

}  // namespace

// ---- the Settings section -----------------------------------------------------------------------------------------

void PagePane::load_automations() {
    std::weak_ptr<int> alive = alive_;
    shell_.call("automations.list", json::object(), [this, alive](Result<json> res) {
        if (!alive.lock()) return;
        if (res) {
            automations_ = *res;
            errors_.erase("automations");
        } else {
            errors_["automations"] = u16(res.error().message);
        }
        rebuild();
    });
}

void PagePane::automations_changed() {
    const RouteName r = shell_.route().name;
    if (r == RouteName::settings) load_automations();
    if (r == RouteName::automation) load();
}

void PagePane::build_automations(Node& c) {
    text(c, shell_.t("settings.automations.body"), Paint::text_secondary);
    const json* list = automations_.is_object() ? jobj(automations_, "triggers") : nullptr;
    if (!list || !list->is_array()) {
        if (!errors_.contains("automations")) text(c, shell_.t("common.loading"), Paint::text_secondary);
    } else if (list->empty()) {
        text(c, shell_.t("settings.automations.empty"), Paint::text_secondary, "settings-automations-empty");
    } else {
        auto* items = c.emplace<Node>("settings-automations");
        items->role = ui::Role::list;
        items->name = shell_.t("settings.automations.title");
        items->gap = 10;
        for (const json& t : *list) {
            const std::string id = jstr(t, "id");
            const std::string tname = jstr(t, "name");
            const std::string disabled = jstr(t, "disabled_reason");
            const bool active = jbool(t, "enabled") && disabled.empty();
            auto* item = items->emplace<Node>("automation-" + id);
            item->role = ui::Role::list_item;
            item->gap = 2;
            const std::u16string kind = label_or(shell_, "settings.automations.kinds." + jstr(t, "kind"), jstr(t, "kind"));
            item->name = u16(tname) + u", " + kind;
            auto* head = item->emplace<Node>();
            head->axis = ui::Axis::row;
            head->gap = 8;
            head->emplace<ui::Label>(shell_.ui(), u16(tname), TextStyle::body_strong, Paint::text, false)->role = ui::Role::none;
            head->emplace<ui::Label>(shell_.ui(), kind, TextStyle::caption, Paint::text_secondary, false)->role = ui::Role::none;
            if (jstr(t, "created_by") == "agent")
                head->emplace<ui::Badge>(shell_.ui(), shell_.t("settings.automations.by_agent"), Paint::info_background, Paint::text,
                                         "automation-by-agent-" + id)
                    ->role = ui::Role::none;
            const auto caption = [&](const std::u16string& s, const std::string& cid) {
                item->emplace<ui::Label>(shell_.ui(), s, TextStyle::caption, Paint::text_secondary, true, cid);
            };
            caption(conditions_text(shell_, t), "automation-conditions-" + id);
            if (!disabled.empty())
                caption(shell_.t("settings.automations.disabled", {{"reason", u16(disabled)}}), "automation-disabled-" + id);
            else if (active && !jstr(t, "next_fire_text").empty())
                caption(shell_.t("settings.automations.next", {{"time", u16(jstr(t, "next_fire_text"))}}), "automation-next-" + id);
            if (const json* last = jobj(t, "last_run"))
                caption(shell_.t("settings.automations.last",
                                 {{"time", u16(jstr(*last, "fire_text"))},
                                  {"state", label_or(shell_, "settings.automations.states." + jstr(*last, "state"), jstr(*last, "state"))}}),
                        "automation-last-" + id);
            else
                caption(shell_.t("settings.automations.never"), "automation-last-" + id);
            auto* actions = item->emplace<Node>();
            actions->axis = ui::Axis::row;
            actions->gap = 8;
            setting_switch(*actions, "automation-enabled-" + id, shell_.t("settings.automations.enabled"), jbool(t, "enabled"), false,
                           [this, id](bool on) {
                               act("automations", "automations.save", {{"id", id}, {"definition", {{"enabled", on}}}},
                                   [this](const json&) { load_automations(); });
                           });
            auto* run = button(*actions, "automation-run-" + id, shell_.t("settings.automations.run_now"), [this, id, tname] {
                act("automations", "automations.run_now", {{"id", id}}, [this, tname](const json&) {
                    shell_.announce(shell_.t("settings.automations.started", {{"name", u16(tname)}}));
                    load_automations();
                });
            });
            run->enabled = disabled.empty() && busy_.empty();
            run->name = shell_.t("settings.automations.run_now_named", {{"name", u16(tname)}});
            button(*actions, "automation-open-" + id, shell_.t("settings.automations.history"), [this, id] { shell_.navigate("#/automations/" + id); },
                   ui::Button::Kind::subtle)
                ->name = shell_.t("settings.automations.history_named", {{"name", u16(tname)}});
            button(*actions, "automation-edit-" + id, shell_.t("settings.automations.edit"),
                   [this, id] { shell_.navigate("#/automations/edit/" + id); }, ui::Button::Kind::subtle)
                ->name = shell_.t("settings.automations.edit_named", {{"name", u16(tname)}});
        }
    }
    error_line(c, "automations");
    button(c, "settings-automations-new", shell_.t("settings.automations.new"), [this] { shell_.navigate("#/automations/new"); });

    // Remote task history.
    c.emplace<ui::Label>(shell_.ui(), shell_.t("settings.automations.remote_title"), TextStyle::body_strong)->role = ui::Role::heading;
    const json* remote = automations_.is_object() ? jobj(automations_, "remote") : nullptr;
    if (!remote || !remote->is_array() || remote->empty()) {
        if (remote) text(c, shell_.t("settings.automations.remote_empty"), Paint::text_secondary, "settings-remote-empty");
        return;
    }
    auto* rl = c.emplace<Node>("settings-remote-tasks");
    rl->role = ui::Role::list;
    rl->name = shell_.t("settings.automations.remote_title");
    rl->gap = 6;
    for (const json& r : *remote) {
        const std::string task = jstr(r, "task_id");
        auto* item = rl->emplace<Node>("remote-" + task);
        item->role = ui::Role::list_item;
        item->gap = 2;
        const std::u16string state = label_or(shell_, "settings.automations.remote_states." + jstr(r, "state"), jstr(r, "state"));
        const std::u16string line = shell_.t("settings.automations.remote_item", {{"issuer", u16(jstr(r, "issuer_name"))}, {"state", state}});
        item->name = line;
        item->emplace<ui::Label>(shell_.ui(), line, TextStyle::body, Paint::text, true)->role = ui::Role::none;
        if (const std::string at = jstr(r, "created_text"); !at.empty())
            item->emplace<ui::Label>(shell_.ui(), u16(at), TextStyle::caption, Paint::text_secondary, true)->role = ui::Role::none;
        if (const std::string note = jstr(r, "note"); !note.empty())
            item->emplace<ui::Label>(shell_.ui(), u16(note), TextStyle::caption, Paint::text_secondary, true, "remote-note-" + task)->role =
                ui::Role::none;
        if (const std::string thread = jstr(r, "thread_id"); !thread.empty())
            button(*item, "remote-open-" + task, shell_.t("settings.automations.open_thread"), [this, thread] { shell_.open_thread(thread); },
                   ui::Button::Kind::subtle);
    }
}

// ---- a Trigger's page -------------------------------------------------------------------------------------------

void PagePane::build_automation() {
    const std::string id = shell_.route().trigger;
    if (!automation_.is_object()) {
        if (errors_.contains("automation")) {
            text(*this, shell_.t("automation.not_found"), Paint::text_secondary, "automation-not-found");
            error_line(*this, "automation");
        } else {
            text(*this, shell_.t("common.loading"), Paint::text_secondary);
        }
        return;
    }
    const json& t = automation_;
    Node* c = card("automation", u16(jstr(t, "name")));
    row(*c, "automation-kind", shell_.t("automation_editor.kind"),
        label_or(shell_, "settings.automations.kinds." + jstr(t, "kind"), jstr(t, "kind")));
    row(*c, "automation-conditions", shell_.t("automation.conditions"), conditions_text(shell_, t));
    row(*c, "automation-prompt", shell_.t("automation.prompt"), u16(jstr(t, "prompt")));
    if (const std::string why = jstr(t, "disabled_reason"); !why.empty())
        row(*c, "automation-disabled", shell_.t("settings.automations.enabled"), shell_.t("settings.automations.disabled", {{"reason", u16(why)}}),
            Paint::warning);
    else if (!jstr(t, "next_fire_text").empty())
        row(*c, "automation-next", shell_.t("automation.next"), u16(jstr(t, "next_fire_text")));
    if (jstr(t, "created_by") == "agent") text(*c, shell_.t("settings.automations.by_agent"), Paint::text_secondary, "automation-by-agent");
    auto* actions = c->emplace<Node>();
    actions->axis = ui::Axis::row;
    actions->gap = 8;
    auto* run = button(*actions, "automation-run", shell_.t("settings.automations.run_now"), [this, id] {
        act("automation", "automations.run_now", {{"id", id}}, [this](const json&) { load(); });
    });
    run->enabled = jstr(t, "disabled_reason").empty() && busy_.empty();
    button(*actions, "automation-edit", shell_.t("settings.automations.edit"), [this, id] { shell_.navigate("#/automations/edit/" + id); },
           ui::Button::Kind::subtle);
    error_line(*c, "automation");

    Node* h = card("automation-history", shell_.t("automation.history_title"));
    const json* hist = jobj(t, "history");
    if (!hist || !hist->is_array() || hist->empty()) {
        text(*h, shell_.t("automation.history_empty"), Paint::text_secondary, "automation-history-empty");
        return;
    }
    auto* list = h->emplace<Node>("automation-history-list");
    list->role = ui::Role::list;
    list->name = shell_.t("automation.history_title");
    list->gap = 8;
    for (const json& o : *hist) {
        const std::string occ = jstr(o, "id");
        const std::u16string reason = label_or(shell_, "settings.automations.reasons." + jstr(o, "reason"), jstr(o, "reason"));
        const std::u16string state = label_or(shell_, "settings.automations.states." + jstr(o, "state"), jstr(o, "state"));
        auto* item = list->emplace<Node>("occurrence-" + occ);
        item->role = ui::Role::list_item;
        item->gap = 2;
        const std::u16string line = u16(jstr(o, "fire_text")) + u", " + reason + u", " + state;
        item->name = line;
        item->emplace<ui::Label>(shell_.ui(), line, TextStyle::body, Paint::text, true)->role = ui::Role::none;
        if (o.contains("duration_ms"))
            item->emplace<ui::Label>(shell_.ui(), shell_.t("automation.duration", {{"seconds", u16(std::to_string((jint(o, "duration_ms") + 500) / 1000))}}),
                                     TextStyle::caption, Paint::text_secondary, true)
                ->role = ui::Role::none;
        if (const json* attempts = jobj(o, "attempts"); attempts && attempts->is_array())
            for (const json& a : *attempts) {
                const std::u16string n = u16(std::to_string(jint(a, "n")));
                const std::string outcome = jstr(a, "outcome", "running");
                const std::u16string os = label_or(shell_, "settings.automations.states." + outcome, outcome);
                const std::string err = jstr(a, "error");
                item->emplace<ui::Label>(shell_.ui(),
                                         err.empty() ? shell_.t("automation.attempt", {{"n", n}, {"outcome", os}})
                                                     : shell_.t("automation.attempt_error", {{"n", n}, {"outcome", os}, {"error", u16(err)}}),
                                         TextStyle::caption, Paint::text_secondary, true, "occurrence-" + occ + "-attempt-" + std::to_string(jint(a, "n")))
                    ->role = ui::Role::none;
            }
        else if (const std::string out = jstr(o, "outcome"); !out.empty())
            item->emplace<ui::Label>(shell_.ui(), u16(out), TextStyle::caption, Paint::text_secondary, true)->role = ui::Role::none;
        // Open Result From History: the Thread at that Run.
        const std::string thread = jstr(o, "thread_id"), run_id = jstr(o, "run_id");
        if (!thread.empty()) {
            std::optional<MessageRef> msg;
            if (!run_id.empty() && jint(o, "msg_seq") > 0) msg = MessageRef{run_id, jint(o, "msg_seq")};
            button(*item, "occurrence-open-" + occ, shell_.t("automation.open_result"), [this, thread, msg] { shell_.navigate(thread_hash(thread, msg)); },
                   ui::Button::Kind::subtle)
                ->name = shell_.t("automation.open_result") + u": " + line;
        }
    }
}

// ---- the editor ---------------------------------------------------------------------------------------------------

AutomationEditor::AutomationEditor(Shell& shell, const Route& route)
    : Node("automation-editor"), shell_(shell), key_(route.hash), id_(route.trigger) {
    gap = 10;
    role = ui::Role::group;
    name = shell_.t(id_.empty() ? "titles.automation_new" : "titles.automation_edit");
    status_ = emplace<ui::Label>(shell_.ui(), u"", TextStyle::body, Paint::text_secondary, true, "automation-editor-status");
    status_->visible = false;
    name_ = emplace<ui::TextField>(shell_.ui(), "automation-name", shell_.t("automation_editor.name"));

    const auto labelled_button = [this](Node& parent, const std::string& id, const std::u16string& label) {
        auto* r = parent.emplace<Node>();
        r->axis = ui::Axis::row;
        r->gap = 12;
        auto* l = r->emplace<ui::Label>(shell_.ui(), label, TextStyle::body, Paint::text, false);
        l->role = ui::Role::none;
        l->padding.top = 6;
        auto* b = r->emplace<ui::Button>(shell_.ui(), id, u"");
        b->width = Sizing::fit;
        return b;
    };
    kind_button_ = labelled_button(*this, "automation-kind", shell_.t("automation_editor.kind"));
    kind_button_->on_click = [this] {
        std::vector<std::pair<std::string, std::u16string>> options;
        for (const auto& k : kinds_) options.emplace_back(k, shell_.t("settings.automations.kinds." + k));
        menu(kind_button_, "automation-kind", options, [this](const std::string& v) {
            kind_ = v;
            show_kind();
        });
    };

    schedule_ = emplace<Node>("automation-schedule");
    schedule_->gap = 6;
    cron_ = schedule_->emplace<ui::TextField>(shell_.ui(), "automation-cron", shell_.t("automation_editor.cron"));
    cron_->set_placeholder(u"0 8 * * 1-5");
    schedule_->emplace<ui::Label>(shell_.ui(), shell_.t("automation_editor.cron_hint"), TextStyle::caption, Paint::text_secondary, true);
    interval_ = schedule_->emplace<ui::TextField>(shell_.ui(), "automation-interval", shell_.t("automation_editor.interval"));
    zone_ = schedule_->emplace<ui::TextField>(shell_.ui(), "automation-zone", shell_.t("automation_editor.zone"));
    zone_->set_placeholder(u"Europe/Prague");

    file_ = emplace<Node>("automation-file");
    file_->gap = 6;
    folder_ = file_->emplace<ui::TextField>(shell_.ui(), "automation-folder", shell_.t("automation_editor.folder"));
    pattern_ = file_->emplace<ui::TextField>(shell_.ui(), "automation-pattern", shell_.t("automation_editor.pattern"));
    pattern_->set_placeholder(u"*.pdf");

    session_ = emplace<Node>("automation-session");
    event_button_ = labelled_button(*session_, "automation-event", shell_.t("automation_editor.event"));
    event_button_->on_click = [this] {
        menu(event_button_, "automation-event",
             {{"logon", shell_.t("automation_editor.events.logon")}, {"unlock", shell_.t("automation_editor.events.unlock")}},
             [this](const std::string& v) {
                 session_event_ = v;
                 show_kind();
             });
    };

    source_ = emplace<Node>("automation-source");
    conditions_ = source_->emplace<ui::TextField>(shell_.ui(), "automation-conditions-json", shell_.t("automation_editor.conditions_json"));

    ui::TextField::Options po;
    po.multiline = true;
    po.min_lines = 3;
    po.max_lines = 10;
    prompt_ = emplace<ui::TextField>(shell_.ui(), "automation-prompt", shell_.t("automation_editor.prompt"), po);
    skill_ = emplace<ui::TextField>(shell_.ui(), "automation-skill", shell_.t("automation_editor.skill"));
    mode_button_ = labelled_button(*this, "automation-thread-mode", shell_.t("automation_editor.thread_mode"));
    mode_button_->on_click = [this] {
        menu(mode_button_, "automation-thread-mode",
             {{"new", shell_.t("automation_editor.modes.new")}, {"append", shell_.t("automation_editor.modes.append")}},
             [this](const std::string& v) {
                 thread_mode_ = v;
                 show_kind();
             });
    };
    notify_ = emplace<ui::Checkbox>(shell_.ui(), "automation-notify", shell_.t("automation_editor.notify"), true);
    notify_->width = Sizing::fit;
    enabled_ = emplace<ui::Checkbox>(shell_.ui(), "automation-enabled", shell_.t("automation_editor.enabled"), true);
    enabled_->width = Sizing::fit;

    auto* actions = emplace<Node>("automation-editor-actions");
    actions->axis = ui::Axis::row;
    actions->gap = 8;
    save_ = actions->emplace<ui::Button>(shell_.ui(), "automation-save", shell_.t("automation_editor.save"), ui::Button::Kind::primary);
    save_->width = Sizing::fit;
    save_->on_click = [this] { save(); };
    auto* back = actions->emplace<ui::Button>(shell_.ui(), "automation-back", shell_.t("automation.back"));
    back->width = Sizing::fit;
    back->on_click = [this] { shell_.navigate("#/settings"); };
    delete_ = actions->emplace<ui::Button>(shell_.ui(), "automation-delete", shell_.t("automation_editor.delete"), ui::Button::Kind::subtle);
    delete_->width = Sizing::fit;
    delete_->on_click = [this] { ask_delete(); };
    delete_->visible = !id_.empty();
    show_kind();
    load();
}

void AutomationEditor::show_status(const std::u16string& text, Paint color) {
    status_->set_text(text);
    status_->color = color;
    status_->visible = !text.empty();
    relayout();
    invalidate();
}

void AutomationEditor::menu(ui::Button* anchor, const std::string& id, std::vector<std::pair<std::string, std::u16string>> options,
                            std::function<void(const std::string&)> pick) {
    auto m = std::make_unique<ui::Menu>(id + "-menu", anchor->name);
    for (const auto& [value, text] : options) m->add_item(shell_.ui(), id + "-" + value, text, [pick, value] { pick(value); });
    shell_.window().overlay().show(std::move(m), anchor->frame(), true);
    shell_.window().render();
}

void AutomationEditor::show_kind() {
    const std::u16string kind = shell_.t("settings.automations.kinds." + kind_);
    kind_button_->set_label(kind + u"  ▾");
    kind_button_->name = shell_.t("automation_editor.kind") + u": " + kind;
    kind_button_->enabled = id_.empty();  // the kind of a saved Trigger stays
    schedule_->visible = kind_ == "schedule";
    file_->visible = kind_ == "file";
    session_->visible = kind_ == "session";
    source_->visible = kind_ == "mail" || kind_ == "calendar" || kind_ == "meeting";
    const std::u16string ev = shell_.t("automation_editor.events." + session_event_);
    event_button_->set_label(ev + u"  ▾");
    event_button_->name = shell_.t("automation_editor.event") + u": " + ev;
    const std::u16string mode = shell_.t("automation_editor.modes." + thread_mode_);
    mode_button_->set_label(mode + u"  ▾");
    mode_button_->name = shell_.t("automation_editor.thread_mode") + u": " + mode;
    save_->enabled = !busy_;
    relayout();
    invalidate();
}

void AutomationEditor::load() {
    std::weak_ptr<int> alive = alive_;
    shell_.call("automations.list", json::object(), [this, alive](Result<json> r) {
        if (!alive.lock() || !r) return;
        if (const json* k = jobj(*r, "kinds"); k && k->is_array()) {
            kinds_.clear();
            for (const auto& x : *k)
                if (x.is_string()) kinds_.push_back(x.get<std::string>());
        }
        if (!id_.empty()) return;
        if (const std::string zone = jstr(*r, "local_zone"); !zone.empty() && zone_->empty()) zone_->set_text(u16(zone));
    });
    if (id_.empty()) return;
    shell_.call("automations.get", {{"id", id_}}, [this, alive](Result<json> r) {
        if (!alive.lock()) return;
        if (!r) return show_status(shell_.t("common.error_prefix") + u" " + u16(r.error().message), Paint::error);
        fill(*r);
    });
}

void AutomationEditor::fill(const json& t) {
    name_->set_text(u16(jstr(t, "name")));
    kind_ = jstr(t, "kind", "schedule");
    const json c = t.contains("conditions") ? t["conditions"] : json::object();
    cron_->set_text(u16(jstr(c, "cron")));
    interval_->set_text(c.contains("interval_minutes") ? u16(std::to_string(jint(c, "interval_minutes"))) : u"");
    zone_->set_text(u16(jstr(c, "time_zone")));
    folder_->set_text(u16(jstr(c, "folder")));
    pattern_->set_text(u16(jstr(c, "pattern")));
    session_event_ = jstr(c, "event", "logon");
    conditions_->set_text(u16(c.dump()));
    prompt_->set_text(u16(jstr(t, "prompt")));
    skill_->set_text(u16(jstr(t, "skill")));
    thread_mode_ = jstr(t, "thread_mode", "new");
    notify_->set_checked(jbool(t, "notify", true));
    enabled_->set_checked(jbool(t, "enabled", true));
    show_kind();
}

json AutomationEditor::definition() const {
    const auto s = [](const ui::TextField* f) { return ui::to_utf8(f->text()); };
    json c = json::object();
    if (kind_ == "schedule") {
        const std::string cron = s(cron_), iv = s(interval_);
        if (!cron.empty()) c["cron"] = cron;
        if (!iv.empty()) {
            long long n = 0;
            for (const char ch : iv) n = std::isdigit(static_cast<unsigned char>(ch)) ? n * 10 + (ch - '0') : -1000000;
            c["interval_minutes"] = n;
        }
        if (const std::string z = s(zone_); !z.empty()) c["time_zone"] = z;
    } else if (kind_ == "file") {
        c["folder"] = s(folder_);
        if (const std::string p = s(pattern_); !p.empty()) c["pattern"] = p;
    } else if (kind_ == "session") {
        c["event"] = session_event_;
    } else {
        c = parse_json(s(conditions_)).value_or(json::object());
    }
    json d = {{"name", s(name_)}, {"kind", kind_},           {"conditions", c},           {"prompt", s(prompt_)},
              {"skill", s(skill_)}, {"thread_mode", thread_mode_}, {"notify", notify_->checked()}, {"enabled", enabled_->checked()}};
    return d;
}

void AutomationEditor::save() {
    if (busy_) return;
    busy_ = true;
    show_kind();
    json p = {{"definition", definition()}};
    if (!id_.empty()) p["id"] = id_;
    std::weak_ptr<int> alive = alive_;
    shell_.call("automations.save", p, [this, alive](Result<json> r) {
        if (!alive.lock()) return;
        busy_ = false;
        show_kind();
        if (!r) return show_status(shell_.t("common.error_prefix") + u" " + u16(r.error().message), Paint::error);
        shell_.announce(shell_.t("automation_editor.saved", {{"name", u16(jstr(*r, "name"))}}));
        shell_.navigate("#/automations/" + jstr(*r, "id"));
    });
}

void AutomationEditor::ask_delete() {
    const std::u16string title = name_->text();
    auto d = std::make_unique<ui::Dialog>(shell_.ui(), "automation-delete-dialog", shell_.t("automation_editor.delete_title", {{"name", title}}));
    d->body().emplace<ui::Label>(shell_.ui(), shell_.t("automation_editor.delete_body"));
    ui::Dialog* dp = d.get();
    std::weak_ptr<int> alive = alive_;
    d->add_button("automation-delete-confirm", shell_.t("automation_editor.delete"), ui::Button::Kind::primary, [this, alive, dp, title] {
        dp->close();
        shell_.call("automations.delete", {{"id", id_}}, [this, alive, title](Result<json> r) {
            if (!alive.lock()) return;
            if (!r) return show_status(shell_.t("common.error_prefix") + u" " + u16(r.error().message), Paint::error);
            shell_.announce(shell_.t("automation_editor.deleted", {{"name", title}}));
            shell_.navigate("#/settings");
        });
    });
    ui::Button* cancel = d->add_button("automation-delete-cancel", shell_.t("settings.cancel"), ui::Button::Kind::standard, [dp] { dp->close(); });
    shell_.window().modal().show(std::move(d));
    shell_.window().tree().set_focus(cancel);  // a destructive action is never the default
    shell_.window().render();
}

}  // namespace wb::shell
