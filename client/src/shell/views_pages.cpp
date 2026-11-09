// Status, Settings, the Telemetry viewer and About (client/work-inbox Status Bar's detail page, Settings Structure, the About
// page; client/telemetry Telemetry Viewer), ported from the web UI's StatusView, SettingsView and AboutView.
#include <algorithm>

#include "shell/derive.h"
#include "shell/format.h"
#include "shell/shell.h"
#include "ui/bundles.h"
#include "ui/text16.h"
#include "ui/textfield.h"

namespace wb::shell {

using ui::Node;
using ui::Paint;
using ui::Sizing;
using ui::TextStyle;

namespace {

constexpr long long kPauseChoices[] = {15, 60, 240};
constexpr long long kVerboseChoices[] = {15, 60, 240};
const char* const kLanguages[] = {"cs", "de", "en"};

// Settings Structure: every section, in order. Each capability fills in its own section as it arrives.
struct Section {
    const char* id;
    const char* title;
    bool later;
};
const Section kSections[] = {
    {"general", "settings.general.title", false},       {"notifications", "settings.notifications.title", false},
    {"files", "settings.files.title", false},            {"connections", "settings.connections.title", false},
    {"mail", "settings.mail.title", false},             {"skills", "settings.skills.title", false},
    {"automations", "settings.automations.title", false}, {"knowledge", "settings.knowledge.title", false},
    {"meetings", "settings.meetings.title", false},      {"privacy", "settings.privacy.title", false},
    {"policy", "settings.policy.title", false},          {"about", "settings.about_section.title", false},
};

std::u16string u16(const std::string& s) { return ui::to_utf16(s); }

// The storage bar: one segment per category, as a share of the quota.
class UsageBar : public Node {
public:
    UsageBar(std::vector<UsageSegment> segs, bool warn) : Node("status-storage-bar"), segs_(std::move(segs)), warn_(warn) {
        role = ui::Role::image;
        height = Sizing::fixed;
        fixed_h = 10;
    }
    void draw(ui::Canvas& c) override {
        static const Paint palette[] = {Paint::accent, Paint::info, Paint::success, Paint::warning, Paint::text_secondary, Paint::error};
        const ui::Rect f = frame();
        c.round(f, 5, Paint::surface_alt, warn_ ? Paint::warning : Paint::border);
        float x = f.x;
        size_t i = 0;
        for (const UsageSegment& s : segs_) {
            const float w = std::min(f.right() - x, static_cast<float>(s.fraction) * f.w);
            if (w > 0) c.fill({x, f.y + 1, w, f.h - 2}, palette[i % std::size(palette)]);
            x += std::max(0.f, w);
            ++i;
        }
    }

private:
    std::vector<UsageSegment> segs_;
    bool warn_;
};

std::u16string minutes_label(Shell& s, long long m) {
    return m % 60 == 0 ? s.count("common.hours", m / 60) : s.count("common.minutes", m);
}

}  // namespace

PagePane::PagePane(Shell& shell) : ScrollView("page"), shell_(shell) {
    role = ui::Role::pane;
    scrolls = true;
    height = Sizing::fill;
    padding = {24, 4, 24, 24};  // under the caption band (improve-window-frame D3)
    gap = 12;
}

// ---- building blocks ---------------------------------------------------------------------------------------------

Node* PagePane::card(const std::string& id, const std::u16string& title) {
    auto* c = emplace<ui::Box>("section-" + id, Paint::surface, 8, Paint::border);
    c->role = ui::Role::group;
    c->name = title;
    c->padding = ui::Insets::all(16);
    c->gap = 8;
    auto* h = c->emplace<ui::Label>(shell_.ui(), title, TextStyle::subtitle, Paint::text, true, "h-" + id);
    h->role = ui::Role::heading;
    h->focusable = true;  // the section links move the keyboard here
    return c;
}

void PagePane::row(Node& parent, const std::string& id, const std::u16string& key, const std::u16string& value, Paint color) {
    auto* r = parent.emplace<Node>(id);
    r->axis = ui::Axis::row;
    r->gap = 12;
    r->role = ui::Role::text;
    r->name = key + u": " + value;
    auto* k = r->emplace<ui::Label>(shell_.ui(), key, TextStyle::body, Paint::text_secondary, true);
    k->role = ui::Role::none;
    k->width = Sizing::fixed;
    k->fixed_w = 200;
    r->emplace<ui::Label>(shell_.ui(), value, TextStyle::body, color, true)->role = ui::Role::none;
}

ui::Label* PagePane::text(Node& parent, const std::u16string& s, Paint color, const std::string& id) {
    return parent.emplace<ui::Label>(shell_.ui(), s, TextStyle::body, color, true, id);
}

ui::Button* PagePane::button(Node& parent, const std::string& id, const std::u16string& label, std::function<void()> f,
                             ui::Button::Kind kind, char16_t glyph) {
    auto* b = parent.emplace<ui::Button>(shell_.ui(), id, label, kind, glyph);
    b->width = Sizing::fit;
    b->on_click = std::move(f);
    return b;
}

// A choice from a short list: a button showing the current value that opens a menu of the options.
void PagePane::choice(Node& parent, const std::string& id, const std::u16string& label, const std::u16string& current,
                      std::vector<std::pair<std::string, std::u16string>> options, std::function<void(const std::string&)> pick,
                      bool can_change) {
    auto* r = parent.emplace<Node>();
    r->axis = ui::Axis::row;
    r->gap = 12;
    auto* l = r->emplace<ui::Label>(shell_.ui(), label, TextStyle::body, Paint::text, false);
    l->role = ui::Role::none;
    l->padding.top = 6;
    auto* b = r->emplace<ui::Button>(shell_.ui(), id, current + u"  ▾");
    b->width = Sizing::fit;
    b->name = label + u": " + current;
    b->enabled = can_change;
    ui::Button* bp = b;
    b->on_click = [this, bp, id, label, options, pick] {
        auto menu = std::make_unique<ui::Menu>(id + "-menu", label);
        for (const auto& [value, text] : options)
            menu->add_item(shell_.ui(), id + "-" + value, text, [pick, value] { pick(value); });
        shell_.window().overlay().show(std::move(menu), bp->frame(), true);
        shell_.window().render();
    };
}

// A switch for one setting; a setting fixed by policy is shown as managed and cannot be changed.
void PagePane::setting_switch(Node& parent, const std::string& id, const std::u16string& label, bool on, bool managed,
                              std::function<void(bool)> change) {
    auto* r = parent.emplace<Node>();
    r->axis = ui::Axis::row;
    r->gap = 12;
    auto* c = r->emplace<ui::Checkbox>(shell_.ui(), id, label, on);
    c->width = Sizing::fit;
    c->enabled = !managed && busy_.empty();
    c->on_change = std::move(change);
    if (managed) {
        c->help = shell_.t("settings.managed");
        r->emplace<ui::Label>(shell_.ui(), shell_.t("settings.managed"), TextStyle::caption, Paint::text_secondary, false,
                              id + "-managed")
            ->padding.top = 4;
    }
}

// Runs a host call as the page's one action in progress; errors show under the page's sections.
void PagePane::act(const std::string& id, const std::string& method, const json& params, std::function<void(const json&)> done) {
    if (!busy_.empty()) return;
    busy_ = id;
    errors_.erase(id);
    std::weak_ptr<int> alive = alive_;
    shell_.call(method, params, [this, alive, id, done = std::move(done)](Result<json> r) {
        if (!alive.lock()) return;
        busy_.clear();
        if (!r) {
            errors_[id] = ui::to_utf16(r.error().message);
            shell_.announce(shell_.t("common.error_prefix") + u" " + errors_[id]);
        } else if (done) {
            done(*r);
        }
        rebuild();
    });
    rebuild();
}

void PagePane::error_line(Node& parent, const std::string& id) {
    if (const auto it = errors_.find(id); it != errors_.end())
        text(parent, shell_.t("common.error_prefix") + u" " + it->second, Paint::error, id + "-error");
}

// ---- data ------------------------------------------------------------------------------------------------------

void PagePane::load() {
    std::weak_ptr<int> alive = alive_;
    const RouteName r = shell_.route().name;
    if (r == RouteName::status) {
        shell_.call("storage.usage", json::object(), [this, alive](Result<json> res) {
            if (!alive.lock()) return;
            usage_ = res ? *res : json(nullptr);
            usage_error_ = res ? u"" : shell_.t("status.storage.load_failed") + u" " + ui::to_utf16(res.error().message);
            rebuild();
        });
    } else if (r == RouteName::settings) {
        shell_.call("settings.get", json::object(), [this, alive](Result<json> res) {
            if (!alive.lock()) return;
            if (res) settings_ = *res;
            else errors_["settings"] = ui::to_utf16(res.error().message);
            rebuild();
        });
        load_skills(false);
        load_automations();
        load_mail();
        load_knowledge();
        load_meetings();
        load_policy();
    } else if (r == RouteName::apps) {
        load_apps();
    } else if (r == RouteName::automation) {
        const std::string id = shell_.route().trigger;
        shell_.call("automations.get", {{"id", id}}, [this, alive, id](Result<json> res) {
            if (!alive.lock() || shell_.route().trigger != id) return;
            automation_ = res ? *res : json(nullptr);
            if (res) errors_.erase("automation");
            else errors_["automation"] = ui::to_utf16(res.error().message);
            rebuild();
        });
    } else if (r == RouteName::telemetry) {
        shell_.call("telemetry.view", json::object(), [this, alive](Result<json> res) {
            if (!alive.lock()) return;
            telemetry_ = res ? *res : json(nullptr);
            telemetry_error_ = res ? u"" : shell_.t("telemetry.load_failed") + u" " + ui::to_utf16(res.error().message);
            rebuild();
        });
    } else if (r == RouteName::about) {
        shell_.call("app.version", json::object(), [this, alive](Result<json> res) {
            if (!alive.lock()) return;
            version_ = res ? *res : json(nullptr);
            version_error_ = res ? u"" : shell_.t("about.load_failed") + u" " + ui::to_utf16(res.error().message);
            rebuild();
        });
    }
}

void PagePane::rebuild() {
    const Route& r = shell_.route();
    const bool page = is_page(r.name);
    const bool entered = page && (r.name != shown_ || r.hash != shown_hash_);
    shown_ = page ? r.name : RouteName::inbox;
    shown_hash_ = page ? r.hash : std::string();
    if (entered) {
        automation_ = nullptr;
        copied_.clear();
        errors_.clear();
        telemetry_shown_ = 100;
        set_scroll_y(0);
        load();
    }
    // The Knowledge view fills the page with its own scrolling panes (improve-knowledge-view D1).
    scrolls = r.name != RouteName::knowledge;
    // Keep the keyboard where it was across the rebuild (status events rebuild the page).
    std::string focus;
    if (ui::Tree* t = Node::tree(); t && t->focused())
        for (const Node* n = t->focused(); n; n = n->parent())
            if (n == this) {
                focus = t->focused()->automation_id();
                break;
            }
    // The Skill editor keeps its fields while its route stays.
    if (editor_) kept_editor_.reset(static_cast<SkillEditor*>(remove(editor_).release()));
    editor_ = nullptr;
    if (auto_editor_) kept_auto_editor_.reset(static_cast<AutomationEditor*>(remove(auto_editor_).release()));
    auto_editor_ = nullptr;
    if (knowledge_view_) kept_knowledge_view_.reset(static_cast<KnowledgeView*>(remove(knowledge_view_).release()));
    knowledge_view_ = nullptr;

    clear_children();
    const char* key = r.name == RouteName::status         ? "titles.status"
                      : r.name == RouteName::knowledge    ? "titles.knowledge"
                      : r.name == RouteName::apps         ? "titles.apps"

                      : r.name == RouteName::settings     ? "titles.settings"
                      : r.name == RouteName::telemetry    ? "titles.telemetry"
                      : r.name == RouteName::skill_editor ? (r.skill.empty() ? "titles.skill_new" : "titles.skill_edit")
                      : r.name == RouteName::automation   ? "titles.automation"
                      : r.name == RouteName::automation_editor ? (r.trigger.empty() ? "titles.automation_new" : "titles.automation_edit")
                                                          : "titles.about";
    name = shell_.t(key);
    auto* head = emplace<Node>("page-head");
    head->axis = ui::Axis::row;
    head->gap = 12;
    auto* h = head->emplace<ui::Label>(shell_.ui(), shell_.t(key), TextStyle::title, Paint::text, true, "page-title");
    h->role = ui::Role::heading;
    if (r.name == RouteName::status) {
        button(*head, "status-refresh", shell_.t("status.refresh"), [this] {
            shell_.refresh_app_status();
            load();
        });
        build_status();
    } else if (r.name == RouteName::settings) {
        build_settings();
    } else if (r.name == RouteName::telemetry) {
        button(*head, "telemetry-back", shell_.t("telemetry.back"), [this] { shell_.navigate("#/settings"); });
        button(*head, "telemetry-refresh", shell_.t("telemetry.refresh"), [this] { load(); });
        build_telemetry();
    } else if (r.name == RouteName::about) {
        text(*this, shell_.t("about.tagline"), Paint::text_secondary, "about-tagline");
        build_about();
    } else if (r.name == RouteName::skill_editor) {
        if (!r.skill.empty()) h->set_text(shell_.t("titles.skill_edit") + u": " + u16(r.skill));
        std::unique_ptr<SkillEditor> e = std::move(kept_editor_);
        if (!e || e->key() != r.hash) e = std::make_unique<SkillEditor>(shell_, r);
        editor_ = static_cast<SkillEditor*>(add(std::move(e)));
    } else if (r.name == RouteName::automation) {
        button(*head, "automation-back", shell_.t("automation.back"), [this] { shell_.navigate("#/settings"); });
        build_automation();
    } else if (r.name == RouteName::automation_editor) {
        std::unique_ptr<AutomationEditor> e = std::move(kept_auto_editor_);
        if (!e || e->key() != r.hash) e = std::make_unique<AutomationEditor>(shell_, r);
        auto_editor_ = static_cast<AutomationEditor*>(add(std::move(e)));
    } else if (r.name == RouteName::apps) {
        button(*head, "apps-refresh", shell_.t("apps.refresh"), [this] { load_apps(); });
        build_apps();
    } else if (r.name == RouteName::knowledge) {
        std::unique_ptr<KnowledgeView> v = std::move(kept_knowledge_view_);
        if (!v) v = std::make_unique<KnowledgeView>(shell_);
        knowledge_view_ = static_cast<KnowledgeView*>(add(std::move(v)));
    }
    if (r.name != RouteName::skill_editor) kept_editor_.reset();
    if (r.name != RouteName::automation_editor) kept_auto_editor_.reset();
    if (r.name != RouteName::knowledge) kept_knowledge_view_.reset();
    relayout();
    invalidate();
    if (!focus.empty())
        if (ui::Tree* t = Node::tree()) {
            std::function<Node*(Node&)> find = [&](Node& n) -> Node* {
                if (n.automation_id() == focus) return &n;
                for (const auto& c : n.children())
                    if (Node* x = find(*c)) return x;
                return nullptr;
            };
            if (Node* n = find(*this); n && n->focusable) t->set_focus(n);
        }
}

// ---- Status --------------------------------------------------------------------------------------------------------

void PagePane::build_status() {
    const json& s = shell_.status();
    if (!is_app_status(s)) {
        text(*this, shell_.t("common.loading"), Paint::text_secondary, "status-loading");
        return;
    }
    const json empty = json::object();
    const json* cpp = jobj(s, "control_plane");
    const json& cp = cpp ? *cpp : empty;
    const json* authp = jobj(s, "auth");
    const json& auth = authp ? *authp : empty;
    const json* bgp = jobj(s, "background");
    const json& bg = bgp ? *bgp : empty;
    const json* stp = jobj(s, "storage");
    const json& st = stp ? *stp : empty;

    // Connection (client/control-channel).
    Node* c = card("connection", shell_.t("status.connection.title"));
    const std::string kind = connection_kind(s);
    const std::string help_key = kind == "not_configured" ? "status.connection.state_help.not_configured"
                                 : kind == "update_required" ? "status.connection.state_help.update_required"
                                                             : "status.connection.state_help." + kind;
    std::u16string state = shell_.t("status.connection." + kind);
    if (shell_.ui().strings.has(help_key)) state += u" — " + shell_.t(help_key);
    row(*c, "status-connection-state", shell_.t("status.connection.state"), state,
        kind == "connected" ? Paint::text : Paint::warning);
    const std::string url = jstr(cp, "url");
    row(*c, "status-connection-server", shell_.t("status.connection.server"), url.empty() ? shell_.t("status.connection.none") : u16(url));
    if (!is_not_configured(s))
        row(*c, "status-connection-push", shell_.t("status.connection.push"),
            shell_.t(jbool(cp, "push_up") ? "status.connection.push_up" : "status.connection.push_down"));
    if (const std::string e = jstr(cp, "config_error"); !e.empty())
        row(*c, "status-connection-config-error", shell_.t("status.connection.config_error"), u16(e), Paint::error);
    if (const std::string v = jstr(cp, "min_version"); !v.empty())
        row(*c, "status-connection-min-version", shell_.t("status.connection.min_version"), u16(v));
    if (const std::string i = jstr(cp, "installation_id"); !i.empty())
        row(*c, "status-connection-installation", shell_.t("status.connection.installation"), u16(i));
    if (cp.contains("last_ack") && cp["last_ack"].is_number())
        row(*c, "status-connection-last-ack", shell_.t("status.connection.last_ack"), u"#" + u16(std::to_string(jint(cp, "last_ack"))));

    // Identity.
    Node* id = card("identity", shell_.t("status.identity.title"));
    const std::string as = jstr(auth, "state");
    const std::string sk = "status.identity.states." + as;
    row(*id, "status-identity-state", shell_.t("status.identity.state"),
        shell_.ui().strings.has(sk) ? shell_.t(sk) : u16(as), as == "signed_in" ? Paint::text : Paint::warning);
    if (const std::string u = auth.contains("user") ? display_user(auth["user"]) : std::string(); !u.empty())
        row(*id, "status-identity-user", shell_.t("status.identity.user"), u16(u));
    if (as == "signed_in" && jint(auth, "access_token_expires_at") > 0)
        row(*id, "status-identity-token", shell_.t("status.identity.token_until"), shell_.clock(jint(auth, "access_token_expires_at")));
    if (const std::string e = jstr(auth, "last_error"); !e.empty())
        row(*id, "status-identity-error", shell_.t("status.identity.last_error"), u16(e));
    if (as == "sign_in_required") {
        button(*id, "status-sign-in", shell_.t("status.identity.sign_in"), [this] { act("sign-in", "identity.sign_in", json::object()); },
               ui::Button::Kind::primary)
            ->enabled = busy_.empty();
        error_line(*id, "sign-in");
    }

    // Background work.
    Node* b = card("background", shell_.t("status.background.title"));
    text(*b, shell_.t("status.background.intro"), Paint::text_secondary);
    const long long now_s = shell_.now() / 1000;
    const long long until = jint(bg, "paused_until");
    const bool paused = until > now_s;
    text(*b,
         paused ? shell_.t("status.background.paused",
                           {{"until", shell_.clock(until)}, {"remaining", u16(format_duration(static_cast<double>(until - now_s)))}})
                : shell_.t("status.background.running"),
         paused ? Paint::warning : Paint::text, "status-background-state");
    auto* pause = b->emplace<Node>("status-pause");
    pause->axis = ui::Axis::row;
    pause->gap = 8;
    pause->role = ui::Role::group;
    pause->name = shell_.t("status.background.pause_group");
    for (long long m : kPauseChoices) {
        const std::u16string label = minutes_label(shell_, m);
        button(*pause, "status-pause-" + std::to_string(m), shell_.t("status.background.pause_for", {{"label", label}}), [this, m, label] {
            act("pause", "background.pause", {{"minutes", m}}, [this, label](const json&) {
                shell_.announce(shell_.t("status.background.paused_for", {{"label", label}}));
                shell_.refresh_app_status();
            });
        })->enabled = busy_.empty();
    }
    if (paused)
        button(*pause, "status-resume", shell_.t("status.background.resume"), [this] {
            act("pause", "background.pause", {{"minutes", 0}}, [this](const json&) {
                shell_.announce(shell_.t("status.background.resumed"));
                shell_.refresh_app_status();
            });
        }, ui::Button::Kind::primary)->enabled = busy_.empty();
    error_line(*b, "pause");
    b->emplace<ui::Label>(shell_.ui(), shell_.t("status.background.deferred_title"), TextStyle::body_strong)->role = ui::Role::heading;
    const auto reasons = deferral_reasons(s);
    if (reasons.empty()) {
        text(*b, shell_.t("status.background.not_deferred"), Paint::text_secondary, "status-not-deferred");
    } else {
        auto* list = b->emplace<Node>("status-deferred");
        list->role = ui::Role::list;
        list->name = shell_.t("status.background.deferred_title");
        for (const std::string& r : reasons)
            text(*list, shell_.t("status.background.deferred." + r), Paint::text, "status-deferred-" + r)->role = ui::Role::list_item;
    }

    // Local storage.
    Node* sc = card("storage", shell_.t("status.storage.title"));
    if (!usage_error_.empty()) {
        text(*sc, usage_error_, Paint::error, "status-storage-error");
    } else if (!usage_.is_object()) {
        text(*sc, shell_.t("common.loading"), Paint::text_secondary, "status-storage-loading");
    } else {
        const double total = static_cast<double>(jint(usage_, "total")), max = static_cast<double>(jint(usage_, "max"));
        const char d = shell_.decimal();
        const double used = max > 0 ? total / max : 0;
        const std::u16string line = max > 0 ? shell_.t("status.storage.used", {{"used", u16(format_bytes(total, d))}, {"max", u16(format_bytes(max, d))}}) +
                                                  u" (" + u16(format_percent(used, d)) + u")"
                                            : u16(format_bytes(total, d));
        text(*sc, line, Paint::text, "status-storage-used");
        if (max > 0) {
            auto* bar = sc->emplace<UsageBar>(usage_segments(usage_["categories"], max), used >= 0.9);
            bar->name = shell_.t("status.storage.bar_label", {{"used", u16(format_bytes(total, d))}, {"max", u16(format_bytes(max, d))},
                                                             {"pct", u16(format_percent(used, d))}});
        }
        if (used >= 0.9) text(*sc, shell_.t("status.storage.nearly_full"), Paint::warning, "status-storage-nearly-full");
        const auto segs = usage_segments(usage_["categories"], max > 0 ? max : total);
        if (segs.empty()) {
            text(*sc, shell_.t("status.storage.empty"), Paint::text_secondary, "status-storage-empty");
        } else {
            auto* list = sc->emplace<Node>("status-storage-categories");
            list->role = ui::Role::list;
            list->name = shell_.t("status.storage.categories");
            for (const UsageSegment& g : segs) row(*list, "status-storage-" + g.name, u16(g.name), u16(format_bytes(static_cast<double>(g.bytes), d)));
            for (const auto& item : list->children()) item->role = ui::Role::list_item;
        }
    }
    const std::u16string mode = jbool(st, "degraded")    ? shell_.t("status.storage.degraded")
                                : jbool(st, "read_only") ? shell_.t("status.storage.read_only")
                                                         : shell_.t("status.storage.writable");
    std::u16string mode_line = mode;
    if (const std::string e = jstr(st, "error"); !e.empty()) mode_line += u" — " + u16(e);
    row(*sc, "status-storage-mode", shell_.t("status.storage.mode"), mode_line,
        jbool(st, "degraded") || jbool(st, "read_only") ? Paint::warning : Paint::text);
    if (const std::string root = jstr(st, "root"); !root.empty())
        row(*sc, "status-storage-location", shell_.t("status.storage.location"), u16(root));
}

// ---- Settings ------------------------------------------------------------------------------------------------------

void PagePane::build_settings() {
    auto* nav = emplace<Node>("settings-sections");
    nav->gap = 2;
    nav->role = ui::Role::group;
    nav->name = shell_.t("settings.sections");
    Node* line = nullptr;
    size_t links = 0;
    for (const Section& s : kSections) {
        const std::string id = s.id;
        if (links++ % 6 == 0) {  // six links a line
            line = nav->emplace<Node>();
            line->axis = ui::Axis::row;
            line->gap = 2;
        }
        button(*line, "settings-jump-" + id, shell_.t(s.title), [this, id] {
            std::function<Node*(Node&)> find = [&](Node& n) -> Node* {
                if (n.automation_id() == "h-" + id) return &n;
                for (const auto& c : n.children())
                    if (Node* x = find(*c)) return x;
                return nullptr;
            };
            ui::Tree* t = Node::tree();
            if (Node* h = find(*this); h && t) {
                t->scroll_to(*this, scroll_y() + (h->frame().y - frame().y) - padding.top);
                t->set_focus(h);
            }
        }, ui::Button::Kind::subtle);
    }
    const json empty = json::object();
    auto setting = [&](const char* key) -> const json& {
        const json* v = settings_.is_object() ? jobj(settings_, key) : nullptr;
        return v ? *v : empty;
    };

    for (const Section& s : kSections) {
        const std::string id = s.id;
        Node* c = card(id, shell_.t(s.title));
        if (s.later) {
            text(*c, shell_.t("settings.later"), Paint::text_secondary, "settings-later-" + id);
            continue;
        }
        if (id == "general") {
            const json& lang = setting("ui.language");
            const std::string current = jstr(lang, "value", "system");
            std::vector<std::pair<std::string, std::u16string>> options{{"system", shell_.t("settings.general.system")}};
            std::u16string current_text = shell_.t("settings.general.system");
            for (const char* l : kLanguages) {
                const std::u16string lname = ui::load_bundle(l).get("settings.general.language_name");
                options.emplace_back(l, lname);
                if (current == l) current_text = lname;
            }
            choice(*c, "settings-language", shell_.t("settings.general.language"), current_text, options,
                   [this](const std::string& v) {
                       act("settings", "settings.set", {{"key", "ui.language"}, {"value", v}}, [this, v](const json& r) {
                           settings_ = r;
                           shell_.apply_language(v);
                       });
                   },
                   settings_.is_object() && !jbool(lang, "managed") && busy_.empty());
            if (jbool(lang, "managed"))
                c->emplace<ui::Label>(shell_.ui(), shell_.t("settings.managed"), TextStyle::caption, Paint::text_secondary, true,
                                      "settings-language-managed");
            c->emplace<ui::Label>(shell_.ui(), shell_.t("settings.general.language_hint"), TextStyle::caption, Paint::text_secondary);
            // Work Details Visibility: the reasoning and tool calls of every Run in the Thread feed.
            if (settings_.is_object()) {
                const json& wd = setting("ui.show_work_details");
                setting_switch(*c, "settings-show-work-details", shell_.t("settings.general.show_work_details"), jbool(wd, "value"),
                               jbool(wd, "managed"), [this](bool on) {
                                   act("settings", "settings.set", {{"key", "ui.show_work_details"}, {"value", on}}, [this, on](const json& r) {
                                       settings_ = r;
                                       shell_.set_show_work_details(on);
                                       shell_.announce(shell_.t("settings.general.saved"));
                                   });
                               });
                c->emplace<ui::Label>(shell_.ui(), shell_.t("settings.general.show_work_details_hint"), TextStyle::caption,
                                      Paint::text_secondary);
            }
            // Dictation language (add-dictation D5): Automatic, or one language without detection.
            if (settings_.is_object() && jbool(shell_.status().value("dictation", json::object()), "enabled")) {
                const std::string dl = jstr(setting("dictation.language"), "value", "auto");
                std::vector<std::pair<std::string, std::u16string>> langs{{"auto", shell_.t("settings.general.dictation_auto")}};
                for (const char* l : {"cs", "de", "en"}) langs.emplace_back(l, shell_.t(std::string("settings.general.dictation_lang.") + l));
                std::u16string shown = langs.front().second;
                for (const auto& [k, v] : langs)
                    if (k == dl) shown = v;
                choice(*c, "settings-dictation-language", shell_.t("settings.general.dictation_language"), shown, langs,
                       [this](const std::string& v) {
                           act("settings", "settings.set", {{"key", "dictation.language"}, {"value", v}}, [this](const json& r) {
                               settings_ = r;
                               shell_.announce(shell_.t("settings.general.saved"));
                           });
                       },
                       busy_.empty());
                c->emplace<ui::Label>(shell_.ui(), shell_.t("settings.general.dictation_language_hint"), TextStyle::caption,
                                      Paint::text_secondary);
            }
            // Account.
            c->emplace<ui::Label>(shell_.ui(), shell_.t("settings.account.title"), TextStyle::body_strong)->role = ui::Role::heading;
            text(*c, shell_.t("settings.account.body"), Paint::text_secondary);
            const std::string as = jstr(jobj(shell_.status(), "auth") ? *jobj(shell_.status(), "auth") : empty, "state");
            const bool signed_out = as == "sign_in_required" || as == "unconfigured";
            auto* out = button(*c, "settings-sign-out", shell_.t("settings.account.sign_out"), [this] { ask_sign_out(); });
            out->enabled = !signed_out;
            if (signed_out) text(*c, shell_.t("settings.account.already_out"), Paint::text_secondary, "settings-signed-out");
        } else if (id == "notifications") {
            text(*c, shell_.t("settings.notifications.body"), Paint::text_secondary);
            if (settings_.is_object())
                for (const char* kind : {"completed", "failed", "action"}) {
                    const std::string k = std::string("notifications.") + kind;
                    const json& v = setting(k.c_str());
                    setting_switch(*c, std::string("notify-") + kind, shell_.t(std::string("settings.notifications.") + kind),
                                   jbool(v, "value", true), jbool(v, "managed"), [this, k](bool on) {
                                       act("settings", "settings.set", {{"key", k}, {"value", on}}, [this](const json& r) {
                                           settings_ = r;
                                           shell_.announce(shell_.t("settings.notifications.saved"));
                                       });
                                   });
                }
            else if (!errors_.contains("settings")) text(*c, shell_.t("common.loading"), Paint::text_secondary);
            error_line(*c, "settings");
        } else if (id == "connections") {
            build_connections(*c);
        } else if (id == "mail") {
            build_mail(*c);
        } else if (id == "knowledge") {
            build_knowledge(*c);
        } else if (id == "meetings") {
            build_meetings(*c);
        } else if (id == "skills") {
            build_skills(*c);
        } else if (id == "automations") {
            build_automations(*c);
        } else if (id == "privacy") {
            build_privacy(*c);
        } else if (id == "policy") {
            build_policy(*c);
        } else if (id == "files") {
            build_files(*c);
        } else if (id == "about") {
            text(*c, shell_.t("settings.about_section.body"), Paint::text_secondary);
            button(*c, "settings-open-about", shell_.t("settings.about_section.open"), [this] { shell_.navigate("#/about"); });
        }
    }
}

void PagePane::build_connections(Node& c) {
    text(c, shell_.t("settings.connections.body"), Paint::text_secondary);
    const json& s = shell_.status();
    const json* mcp = s.contains("mcp") && s["mcp"].is_array() ? &s["mcp"] : nullptr;
    if (!mcp || mcp->empty()) {
        text(c, shell_.t("settings.connections.empty"), Paint::text_secondary, "settings-connections-empty");
    } else {
        auto* list = c.emplace<Node>("settings-servers");
        list->role = ui::Role::list;
        list->name = shell_.t("settings.connections.title");
        list->gap = 8;
        for (const json& srv : *mcp) {
            const std::string id = jstr(srv, "id");
            const std::string state = jstr(srv, "state");
            auto* item = list->emplace<Node>("server-" + id);
            item->role = ui::Role::list_item;
            item->gap = 2;
            const std::string sk = "settings.connections.states." + state;
            const std::u16string st = shell_.ui().strings.has(sk) ? shell_.t(sk) : u16(state);
            const std::u16string where = shell_.t(jstr(srv, "kind") == "local" ? "settings.connections.local" : "settings.connections.remote");
            item->name = u16(jstr(srv, "name")) + u", " + where + u", " + st;
            auto* head = item->emplace<Node>();
            head->axis = ui::Axis::row;
            head->gap = 8;
            head->emplace<ui::Label>(shell_.ui(), u16(jstr(srv, "name")), TextStyle::body_strong, Paint::text, false)->role = ui::Role::none;
            head->emplace<ui::Label>(shell_.ui(), where, TextStyle::caption, Paint::text_secondary, false)->role = ui::Role::none;
            head->emplace<ui::Badge>(shell_.ui(), st,
                                     state == "failed"                                              ? Paint::error_background
                                     : state == "unavailable" || !jstr(srv, "warning").empty()      ? Paint::warning_background
                                     : state == "connected"                                         ? Paint::info_background
                                                                                                    : Paint::surface_alt,
                                     Paint::text)
                ->role = ui::Role::none;
            item->emplace<ui::Label>(shell_.ui(),
                                     shell_.t("settings.connections.tools", {{"usable", u16(std::to_string(jint(srv, "usable_tools")))},
                                                                             {"total", u16(std::to_string(jint(srv, "tools")))}}),
                                     TextStyle::caption, Paint::text_secondary)
                ->role = ui::Role::none;
            if (const std::string w = jstr(srv, "warning"); !w.empty())
                item->emplace<ui::Label>(shell_.ui(), shell_.t("settings.connections.warning") + u": " + u16(w), TextStyle::caption)->role = ui::Role::none;
            if (const std::string e = jstr(srv, "error"); !e.empty())
                item->emplace<ui::Label>(shell_.ui(), shell_.t("settings.connections.problem") + u": " + u16(e), TextStyle::caption)->role = ui::Role::none;
        }
    }
    // The agent browser (client/browser-use Chrome Policy Compatibility).
    if (const json* b = jobj(s, "browser")) {
        const std::string state = jstr(*b, "state");
        const std::string sk = "settings.connections.browser.states." + state;
        const std::u16string st = shell_.ui().strings.has(sk) ? shell_.t(sk) : u16(state);
        auto* item = c.emplace<Node>("settings-browser");
        item->role = ui::Role::group;
        item->gap = 2;
        std::u16string detail;
        const std::u16string version = u16(jstr(*b, "version"));
        if (state == "available")
            detail = jbool(*b, "running") ? shell_.t(jbool(*b, "headless") ? "settings.connections.browser.detail_headless"
                                                                           : "settings.connections.browser.detail_running",
                                                     {{"version", version}})
                                          : shell_.t("settings.connections.browser.detail_idle", {{"version", version}});
        else if (state == "blocked")
            detail = shell_.t("settings.connections.problem") + u": " +
                     shell_.t("settings.connections.browser.blocked", {{"policy", u16(jstr(*b, "policy"))}});
        else if (const std::string r = jstr(*b, "reason"); !r.empty())
            detail = shell_.t("settings.connections.problem") + u": " + u16(r);
        item->name = shell_.t("settings.connections.browser.name") + u", " + st;
        auto* head = item->emplace<Node>();
        head->axis = ui::Axis::row;
        head->gap = 8;
        head->emplace<ui::Label>(shell_.ui(), shell_.t("settings.connections.browser.name"), TextStyle::body_strong, Paint::text, false)->role =
            ui::Role::none;
        head->emplace<ui::Badge>(shell_.ui(), st, state == "available" ? Paint::info_background : state == "blocked" ? Paint::error_background : Paint::warning_background,
                                 Paint::text)
            ->role = ui::Role::none;
        if (!detail.empty()) item->emplace<ui::Label>(shell_.ui(), detail, TextStyle::caption, Paint::text_secondary, true, "settings-browser-detail");
    }
}

void PagePane::load_skills(bool reload) {
    std::weak_ptr<int> alive = alive_;
    shell_.call("skills.catalog", json{{"reload", reload}}, [this, alive](Result<json> res) {
        if (!alive.lock()) return;
        if (res) {
            skills_ = *res;
            errors_.erase("skills");
        } else {
            errors_["skills"] = ui::to_utf16(res.error().message);
        }
        rebuild();
    });
}

// The Skills section (client/skills Skill Load Tiers, Skill Tool Scope, Skill MCP Requirements): every name with the
// copy in use, its tier, version, risk and tools, and what each other copy is (shadowed, conflicting, invalid).
void PagePane::build_skills(Node& c) {
    text(c, shell_.t("settings.skills.body"), Paint::text_secondary);
    const auto label = [this](const std::string& prefix, const std::string& value) {
        const std::string k = prefix + value;
        return shell_.ui().strings.has(k) ? shell_.t(k) : u16(value);
    };
    const auto joined = [](const json& a) {
        std::string s;
        if (a.is_array())
            for (const auto& x : a)
                if (x.is_string()) s += (s.empty() ? "" : ", ") + x.get<std::string>();
        return s;
    };
    const json* list = skills_.is_object() ? jobj(skills_, "skills") : nullptr;
    if (!list || !list->is_array()) {
        if (!errors_.contains("skills")) text(c, shell_.t("common.loading"), Paint::text_secondary);
    } else if (list->empty()) {
        text(c, shell_.t("settings.skills.empty"), Paint::text_secondary, "settings-skills-empty");
    } else {
        auto* items = c.emplace<Node>("settings-skills");
        items->role = ui::Role::list;
        items->name = shell_.t("settings.skills.title");
        items->gap = 8;
        for (const json& s : *list) {
            const std::string skill = jstr(s, "name");
            const std::string state = jstr(s, "state");
            auto* item = items->emplace<Node>("skill-entry-" + skill);
            item->role = ui::Role::list_item;
            item->gap = 2;
            const std::u16string tier = label("settings.skills.tiers.", jstr(s, "tier"));
            const std::u16string st = label("settings.skills.states.", state);
            item->name = u16(skill) + u", " + tier + u", " + st;
            auto* head = item->emplace<Node>();
            head->axis = ui::Axis::row;
            head->gap = 8;
            head->emplace<ui::Label>(shell_.ui(), u16(skill), TextStyle::body_strong, Paint::text, false)->role = ui::Role::none;
            head->emplace<ui::Label>(shell_.ui(), tier, TextStyle::caption, Paint::text_secondary, false)->role = ui::Role::none;
            if (const std::string v = jstr(s, "version"); !v.empty())
                head->emplace<ui::Label>(shell_.ui(), shell_.t("settings.skills.version", {{"version", u16(v)}}), TextStyle::caption,
                                         Paint::text_secondary, false)
                    ->role = ui::Role::none;
            head->emplace<ui::Badge>(shell_.ui(), st,
                                     state == "invalid"       ? Paint::error_background
                                     : state == "unavailable" ? Paint::warning_background
                                                              : Paint::info_background,
                                     Paint::text, "skill-state-" + skill)
                ->role = ui::Role::none;
            const auto caption = [&](const std::u16string& t, const std::string& id = {}) {
                item->emplace<ui::Label>(shell_.ui(), t, TextStyle::caption, Paint::text_secondary, true, id);  // read out
            };
            if (const std::string d = jstr(s, "description"); !d.empty())
                item->emplace<ui::Label>(shell_.ui(), u16(d), TextStyle::body, Paint::text, true)->role = ui::Role::none;
            if (const std::string src = jstr(s, "source"); !src.empty()) caption(shell_.t("settings.skills.source", {{"source", u16(src)}}));
            if (const json* e = jobj(s, "error"); e && e->is_object())
                caption(shell_.t("settings.skills.error", {{"field", u16(jstr(*e, "field"))}, {"message", u16(jstr(*e, "message"))}}),
                        "skill-error-" + skill);
            if (const std::string why = jstr(s, "unavailable"); !why.empty())
                caption(shell_.t("settings.skills.unavailable", {{"reason", u16(why)}}), "skill-unavailable-" + skill);
            if (state != "invalid") {
                const std::string risk = jstr(s, "risk");
                if (!risk.empty()) caption(shell_.t("settings.skills.risk", {{"risk", u16(risk)}}));
                const std::string tools = joined(s.contains("tools") ? s["tools"] : json());
                caption(tools.empty() ? shell_.t("settings.skills.no_tools") : shell_.t("settings.skills.tools", {{"tools", u16(tools)}}),
                        "skill-tools-" + skill);
                if (const std::string missing = joined(s.contains("unavailable_tools") ? s["unavailable_tools"] : json()); !missing.empty())
                    caption(shell_.t("settings.skills.unavailable_tools", {{"tools", u16(missing)}}), "skill-unavailable-tools-" + skill);
            }
            // Registry Skills: a pin, a pending update and the version to roll back to (Skill Updates And Rollback).
            if (jbool(s, "held")) caption(shell_.t("settings.skills.pinned"), "skill-pinned-" + skill);
            const std::string tier_id = jstr(s, "tier");
            if (const std::string offer = jstr(s, "offer"); !offer.empty())
                button(*item, "skill-update-" + skill, shell_.t("settings.skills.update", {{"version", u16(offer)}}), [this, skill, tier_id] {
                    act("skills", "skills.update", {{"tier", tier_id}, {"name", skill}}, [this](const json&) { load_skills(true); });
                });
            if (const std::string prev = jstr(s, "previous"); !prev.empty())
                button(*item, "skill-rollback-" + skill, shell_.t("settings.skills.rollback", {{"version", u16(prev)}}), [this, skill, tier_id] {
                    act("skills", "skills.rollback", {{"tier", tier_id}, {"name", skill}}, [this](const json&) { load_skills(true); });
                }, ui::Button::Kind::subtle);
            // Skill Authoring: the user's own Skills open in the editor.
            if (tier_id == "private")
                button(*item, "skill-edit-" + skill, shell_.t("settings.skills.edit"), [this, skill] { shell_.navigate("#/skills/edit/" + skill); },
                       ui::Button::Kind::subtle)
                    ->name = shell_.t("settings.skills.edit_named", {{"name", u16(skill)}});
            if (const json* others = jobj(s, "others"); others && others->is_array()) {
                size_t i = 0;
                for (const json& o : *others) {
                    std::u16string line = shell_.t("settings.skills.other", {{"tier", label("settings.skills.tiers.", jstr(o, "tier"))},
                                                                             {"state", label("settings.skills.states.", jstr(o, "state"))}});
                    if (const json* e = jobj(o, "error"); e && e->is_object())
                        line += u" — " + shell_.t("settings.skills.error", {{"field", u16(jstr(*e, "field"))}, {"message", u16(jstr(*e, "message"))}});
                    caption(line, "skill-other-" + skill + "-" + std::to_string(i++));
                }
            }
        }
    }
    error_line(c, "skills");
    auto* actions = c.emplace<Node>();
    actions->axis = ui::Axis::row;
    actions->gap = 8;
    button(*actions, "settings-skills-new", shell_.t("settings.skills.new"), [this] { shell_.navigate("#/skills/new"); });
    button(*actions, "settings-skills-reload", shell_.t("settings.skills.reload"), [this] { load_skills(true); }, ui::Button::Kind::subtle);
}

void PagePane::load_policy() {
    std::weak_ptr<int> alive = alive_;
    shell_.call("policy.summary", json::object(), [this, alive](Result<json> res) {
        if (!alive.lock()) return;
        if (res) {
            policy_ = *res;
            errors_.erase("policy");
        } else {
            errors_["policy"] = ui::to_utf16(res.error().message);
        }
        rebuild();
    });
}

namespace {
// A policy value for display: strings as they are, lists joined, maps as key = value pairs.
std::string policy_value(const json& v) {
    if (v.is_null()) return {};
    if (v.is_string()) return v.get<std::string>();
    if (v.is_array()) {
        std::string s;
        for (const auto& x : v) s += (s.empty() ? "" : ", ") + policy_value(x);
        return s;
    }
    if (v.is_object()) {
        std::string s;
        for (const auto& [k, x] : v.items()) s += (s.empty() ? "" : "; ") + k + " = " + policy_value(x);
        return s;
    }
    return v.dump();
}
}  // namespace

// The Policy page (improve-usability: client/work-inbox Policy Page): what the policy in force allows, and where each
// setting comes from (client/policy-enforcement Policy Visibility To User).
void PagePane::build_policy(Node& c) {
    text(c, shell_.t("settings.policy.body"), Paint::text_secondary);
    if (!policy_.is_object()) {
        if (!errors_.contains("policy")) text(c, shell_.t("common.loading"), Paint::text_secondary);
        error_line(c, "policy");
        return;
    }
    const json empty = json::object();
    const json* srcp = jobj(policy_, "sources");
    const json& src = srcp && srcp->is_object() ? *srcp : empty;
    // The Group or guardrail a key comes from: the key itself or its nearest parent in the bundle's sources.
    const auto source_of = [&](std::string key) -> std::u16string {
        for (;;) {
            if (const std::string s = jstr(src, key.c_str()); !s.empty()) return u16(s);
            const auto dot = key.rfind('.');
            if (dot == std::string::npos) return shell_.t("settings.policy.default");
            key.resize(dot);
        }
    };
    const auto none = shell_.t("settings.policy.none");
    const auto setting = [&](const std::string& id, const char* label, const json& v, const std::string& key) {
        const std::string s = policy_value(v);
        row(c, "policy-" + id, shell_.t(label), (s.empty() ? none : u16(s)) + u"  · " + source_of(key));
    };
    const std::string state = jstr(policy_, "state");
    row(c, "policy-state", shell_.t("settings.policy.state"), shell_.t("settings.policy.states." + (state.empty() ? std::string("none") : state)),
        state == "valid" ? Paint::text : Paint::warning);
    if (state != "none") {
        row(c, "policy-version", shell_.t("settings.policy.version"),
            u16(std::to_string(jint(policy_, "version"))) + u" — " + shell_.t("settings.policy.expires", {{"time", shell_.clock(jint(policy_, "expires_at"))}}));
        row(c, "policy-groups", shell_.t("settings.policy.groups"), u16(policy_value(policy_.value("groups", json::array()))));
    }
    if (const std::string e = jstr(policy_, "explanation"); !e.empty()) text(c, u16(e), Paint::warning, "policy-explanation");
    if (const std::string e = jstr(policy_, "last_error"); !e.empty()) row(c, "policy-last-error", shell_.t("settings.policy.last_error"), u16(e), Paint::warning);
    if (const json* tools = jobj(policy_, "tools"); tools && tools->is_object()) {
        c.emplace<ui::Label>(shell_.ui(), shell_.t("settings.policy.tools_title"), TextStyle::body_strong)->role = ui::Role::heading;
        const std::string cm = jstr(*tools, "computer_mode", "off"), sm = jstr(*tools, "shell_mode", "off");
        row(c, "policy-computer", shell_.t("settings.policy.computer"),
            shell_.t(cm == "off" ? "settings.policy.off" : "settings.policy.on") + u"  · " + source_of("tools.computer.mode"),
            cm == "off" ? Paint::warning : Paint::text);
        const char* shell_key = sm == "off" ? "settings.policy.off" : sm == "allow" ? "settings.policy.shell_allow" : "settings.policy.shell_sandboxed";
        std::u16string shell = shell_.t(shell_key) + u"  · " + source_of("tools.shell.mode");
        if (const std::string why = jstr(*tools, "shell_unavailable"); !why.empty()) shell += u" — " + u16(why);
        row(c, "policy-shell", shell_.t("settings.policy.shell"), shell, sm == "off" ? Paint::warning : Paint::text);
        setting("risk", "settings.policy.risk", tools->value("risk", json()), "tools.risk");
        setting("rules", "settings.policy.rules", tools->value("rules", json()), "tools.rules");
        if (cm == "off" || sm == "off") text(c, shell_.t("settings.policy.ask_admin"), Paint::text_secondary, "policy-ask-admin");
    }
    if (policy_.contains("models")) setting("models", "settings.policy.models", policy_["models"], "llm.models");
    if (const json* fs = jobj(policy_, "filesystem"); fs && fs->is_object()) {
        c.emplace<ui::Label>(shell_.ui(), shell_.t("settings.policy.files_title"), TextStyle::body_strong)->role = ui::Role::heading;
        for (const char* k : {"read", "write", "deny", "grantable"})
            setting(std::string("fs-") + k, ("settings.policy.fs." + std::string(k)).c_str(), fs->value(k, json()), std::string("filesystem.") + k);
    }
    if (const json* net = jobj(policy_, "network"); net && net->is_object()) {
        c.emplace<ui::Label>(shell_.ui(), shell_.t("settings.policy.network_title"), TextStyle::body_strong)->role = ui::Role::heading;
        setting("net-allow", "settings.policy.net.allow", net->value("egress_allow", json()), "network.egress_allow");
        setting("net-deny", "settings.policy.net.deny", net->value("egress_deny", json()), "network.egress_deny");
        row(c, "policy-net-enforcement", shell_.t("settings.policy.net.enforcement"), u16(policy_value(net->value("enforcement", json()))));
    }
    if (const json* un = jobj(policy_, "unattended"); un && un->is_object()) {
        c.emplace<ui::Label>(shell_.ui(), shell_.t("settings.policy.unattended_title"), TextStyle::body_strong)->role = ui::Role::heading;
        setting("un-risk", "settings.policy.un.risk", un->value("allowed_risk", json()), "unattended.allowed_risk");
        setting("un-rules", "settings.policy.un.rules", un->value("rules", json()), "unattended.rules");
    }
    button(c, "policy-refresh", shell_.t("settings.policy.refresh"), [this] {
        act("policy", "policy.refresh", json::object(), [this](const json&) {
            shell_.announce(shell_.t("settings.policy.refreshed"));
            load_policy();
        });
    })->enabled = busy_.empty();
    error_line(c, "policy");
}

// The Files section (improve-usability: client/work-inbox Files Settings): the user's folder grants within
// policy.filesystem.grantable (client/policy-enforcement User Folder Grants).
void PagePane::build_files(Node& c) {
    text(c, shell_.t("settings.files.body"), Paint::text_secondary);
    if (!policy_.is_object()) {
        if (!errors_.contains("policy")) text(c, shell_.t("common.loading"), Paint::text_secondary);
        return;
    }
    const json* fs = jobj(policy_, "filesystem");
    const std::string grantable = fs ? policy_value(fs->value("grantable", json())) : std::string();
    text(c, grantable.empty() ? shell_.t("settings.files.none_grantable") : shell_.t("settings.files.grantable", {{"folders", u16(grantable)}}),
         Paint::text_secondary, "files-grantable");
    const json* grants = jobj(policy_, "grants");
    if (!grants || !grants->is_array() || grants->empty()) {
        text(c, shell_.t("settings.files.empty"), Paint::text_secondary, "files-empty");
    } else {
        auto* list = c.emplace<Node>("files-grants");
        list->role = ui::Role::list;
        list->name = shell_.t("settings.files.title");
        list->gap = 4;
        size_t i = 0;
        for (const json& g : *grants) {
            const std::string folder = jstr(g, "folder");
            const std::u16string access = shell_.t(jbool(g, "write") ? "settings.files.read_write" : "settings.files.read_only");
            auto* item = list->emplace<Node>("files-grant-" + std::to_string(i));
            item->role = ui::Role::list_item;
            item->name = u16(folder) + u", " + access;
            item->axis = ui::Axis::row;
            item->gap = 12;
            item->emplace<ui::Label>(shell_.ui(), u16(folder), TextStyle::body, Paint::text, true)->role = ui::Role::none;
            item->emplace<ui::Badge>(shell_.ui(), access, Paint::info_background, Paint::text)->role = ui::Role::none;
            button(*item, "files-revoke-" + std::to_string(i++), shell_.t("settings.files.revoke"), [this, folder] {
                act("files", "policy.grants.remove", {{"folder", folder}}, [this](const json&) {
                    shell_.announce(shell_.t("settings.files.revoked"));
                    load_policy();
                });
            }, ui::Button::Kind::subtle)->name = shell_.t("settings.files.revoke_named", {{"folder", u16(folder)}});
        }
    }
    auto* actions = c.emplace<Node>();
    actions->axis = ui::Axis::row;
    actions->gap = 8;
    for (const bool write : {false, true}) {
        button(*actions, write ? "files-add-write" : "files-add-read", shell_.t(write ? "settings.files.add_write" : "settings.files.add_read"), [this, write] {
            const std::wstring w = shell_.pick_folder();
            if (w.empty()) return;
            act("files", "policy.grants.add", {{"folder", ui::to_utf8(std::u16string(w.begin(), w.end()))}, {"write", write}}, [this](const json&) {
                shell_.announce(shell_.t("settings.files.granted"));
                load_policy();
            });
        })->enabled = busy_.empty() && !grantable.empty();
    }
    error_line(c, "files");
}

void PagePane::build_privacy(Node& c) {
    const json* p = jobj(shell_.status(), "policy");
    std::u16string policy = shell_.t("settings.privacy.policy_none");
    if (p && !jstr(*p, "version").empty()) {
        std::string groups;
        if (p->contains("groups") && (*p)["groups"].is_array())
            for (const json& g : (*p)["groups"])
                if (g.is_string()) groups += (groups.empty() ? "" : ", ") + g.get<std::string>();
        policy = shell_.t("settings.privacy.policy_line", {{"version", u16(jstr(*p, "version"))}, {"groups", u16(groups)}});
    }
    row(c, "settings-policy", shell_.t("settings.privacy.policy"), policy);

    // Telemetry (client/telemetry Telemetry Viewer).
    c.emplace<ui::Label>(shell_.ui(), shell_.t("settings.privacy.telemetry"), TextStyle::body_strong)->role = ui::Role::heading;
    text(c, shell_.t("settings.privacy.telemetry_body"), Paint::text_secondary);
    if (const json* tel = jobj(shell_.status(), "telemetry"))
        row(c, "settings-telemetry-level", shell_.t("telemetry.level.level"), shell_.t("telemetry.levels." + jstr(*tel, "level", "metadata")));
    button(c, "settings-open-telemetry", shell_.t("settings.privacy.telemetry_open"), [this] { shell_.navigate("#/settings/telemetry"); });

    // Diagnostics.
    c.emplace<ui::Label>(shell_.ui(), shell_.t("settings.diagnostics.title"), TextStyle::body_strong)->role = ui::Role::heading;
    text(c, shell_.t("settings.diagnostics.body"), Paint::text_secondary);
    const long long now_s = shell_.now() / 1000;
    const json* diag = jobj(shell_.status(), "diagnostics");
    const long long until = diag && diag->contains("verbose_until") ? jint(*diag, "verbose_until") : verbose_until_;
    const bool on = until > now_s;
    setting_switch(c, "settings-verbose", shell_.t("settings.diagnostics.toggle"), on, false, [this, on](bool) {
        const long long next = on ? 0 : verbose_minutes_;
        act("verbose", "diagnostics.verbose", {{"minutes", next}}, [this, next](const json&) {
            verbose_until_ = next > 0 ? shell_.now() / 1000 + next * 60 : 0;
            shell_.announce(shell_.t(next > 0 ? "settings.diagnostics.enabled_announce" : "settings.diagnostics.disabled_announce"));
            shell_.refresh_app_status();
        });
    });
    text(c, on ? shell_.t("settings.diagnostics.on", {{"until", shell_.clock(until)}}) : shell_.t("settings.diagnostics.off"),
         Paint::text_secondary, "settings-verbose-state");
    std::vector<std::pair<std::string, std::u16string>> durations;
    for (long long m : kVerboseChoices) durations.emplace_back(std::to_string(m), minutes_label(shell_, m));
    choice(c, "settings-verbose-for", shell_.t("settings.diagnostics.duration"), minutes_label(shell_, verbose_minutes_), durations,
           [this](const std::string& v) {
               verbose_minutes_ = std::stoll(v);
               rebuild();
           },
           !on && busy_.empty());
    error_line(c, "verbose");

    // Delete everything.
    c.emplace<ui::Label>(shell_.ui(), shell_.t("settings.privacy.delete"), TextStyle::body_strong)->role = ui::Role::heading;
    text(c, shell_.t("settings.privacy.body"), Paint::text_secondary);
    button(c, "settings-delete-everything", shell_.t("settings.privacy.delete"), [this] { ask_wipe(); })->enabled = !wiped_;
    if (wiped_) text(c, shell_.t("settings.privacy.done"), Paint::text, "settings-wiped");
}

void PagePane::ask_sign_out() {
    auto d = std::make_unique<ui::Dialog>(shell_.ui(), "sign-out-dialog", shell_.t("settings.account.confirm_title"));
    d->body().emplace<ui::Label>(shell_.ui(), shell_.t("settings.account.confirm_body"));
    auto* err = d->body().emplace<ui::Label>(shell_.ui(), u"", TextStyle::body, Paint::error, true, "sign-out-error");
    err->visible = false;
    ui::Dialog* dp = d.get();
    ui::Button* cancel = d->add_button("sign-out-cancel", shell_.t("settings.cancel"), ui::Button::Kind::standard, [dp] { dp->close(); });
    std::weak_ptr<int> alive = alive_;
    d->add_button("sign-out-confirm", shell_.t("settings.account.confirm"), ui::Button::Kind::primary, [this, alive, dp, err] {
        shell_.call("identity.sign_out", json::object(), [this, alive, dp, err](Result<json> r) {
            if (!alive.lock() || shell_.window().modal().top() != dp) return;
            if (!r) {
                err->set_text(ui::to_utf16(r.error().message));
                err->visible = true;
                err->relayout();
                return;
            }
            dp->close();
            shell_.refresh_app_status();
        });
    });
    shell_.window().modal().show(std::move(d));
    shell_.window().tree().set_focus(cancel);
    shell_.window().render();
}

void PagePane::ask_wipe() {
    auto d = std::make_unique<ui::Dialog>(shell_.ui(), "wipe-dialog", shell_.t("settings.privacy.confirm_title"));
    d->body().emplace<ui::Label>(shell_.ui(), shell_.t("settings.privacy.confirm_body"));
    const std::u16string word = shell_.t("settings.privacy.confirm_word");
    auto* field = d->body().emplace<ui::TextField>(shell_.ui(), "wipe-confirm",
                                                   shell_.t("settings.privacy.type_to_confirm", {{"word", word}}));
    auto* err = d->body().emplace<ui::Label>(shell_.ui(), u"", TextStyle::body, Paint::error, true, "wipe-error");
    err->visible = false;
    ui::Dialog* dp = d.get();
    d->add_button("wipe-cancel", shell_.t("settings.cancel"), ui::Button::Kind::standard, [dp] { dp->close(); });
    std::weak_ptr<int> alive = alive_;
    auto typed_ok = [field, word] {
        std::u16string t = field->text();
        while (!t.empty() && (t.back() == u' ' || t.back() == u'\t')) t.pop_back();
        while (!t.empty() && (t.front() == u' ' || t.front() == u'\t')) t.erase(t.begin());
        return t == word;
    };
    ui::Button* confirm = nullptr;
    auto run = [this, alive, dp, err, typed_ok] {
        if (!typed_ok()) return;
        shell_.call("storage.delete_everything", {{"confirm", true}}, [this, alive, dp, err](Result<json> r) {
            if (!alive.lock() || shell_.window().modal().top() != dp) return;
            if (!r) {
                err->set_text(ui::to_utf16(r.error().message));
                err->visible = true;
                err->relayout();
                return;
            }
            dp->close();
            wiped_ = true;
            shell_.announce(shell_.t("settings.privacy.done"));
            rebuild();
        });
    };
    confirm = d->add_button("wipe-confirm-button", shell_.t("settings.privacy.confirm"), ui::Button::Kind::primary, run);
    confirm->enabled = false;
    field->on_change = [confirm, typed_ok] {
        confirm->enabled = typed_ok();
        confirm->invalidate();
    };
    field->on_key_down = [run](const ui::KeyEvent& e) {
        if (e.key != ui::kKeyEnter || e.shift || e.ctrl || e.alt) return false;
        run();
        return true;
    };
    shell_.window().modal().show(std::move(d));
    shell_.window().tree().set_focus(field);
    shell_.window().render();
}

// ---- Telemetry viewer ------------------------------------------------------------------------------------------------

void PagePane::build_telemetry() {
    if (!telemetry_error_.empty()) {
        text(*this, telemetry_error_, Paint::error, "telemetry-error");
        return;
    }
    if (!telemetry_.is_object()) {
        text(*this, shell_.t("common.loading"), Paint::text_secondary, "telemetry-loading");
        return;
    }
    const json& v = telemetry_;
    const std::string level = jstr(v, "level", "metadata");
    Node* c = card("telemetry-level", shell_.t("telemetry.level.title"));
    row(*c, "telemetry-level", shell_.t("telemetry.level.level"), shell_.t("telemetry.levels." + level),
        level == "content" ? Paint::warning : Paint::text);
    text(*c, shell_.t("telemetry.levels_help." + level), Paint::text_secondary, "telemetry-level-help");
    if (jbool(v, "content_active")) text(*c, shell_.t("telemetry.level.content_active"), Paint::warning, "telemetry-content-active");
    const std::string source = jstr(v, "source", "default");
    row(*c, "telemetry-source", shell_.t("telemetry.level.source"),
        source == "default" ? shell_.t("telemetry.level.default_source") : u16(source));
    if (const long long pv = jint(v, "policy_version"); pv > 0)
        row(*c, "telemetry-policy-version", shell_.t("telemetry.level.policy_version"), u16(std::to_string(pv)));
    row(*c, "telemetry-pending", shell_.t("telemetry.level.pending"), u16(std::to_string(jint(v, "pending"))));
    if (const long long d = jint(v, "discarded"); d > 0)
        row(*c, "telemetry-discarded", shell_.t("telemetry.level.discarded"), u16(std::to_string(d)), Paint::warning);
    row(*c, "telemetry-crash-reports", shell_.t("telemetry.level.crash_reports"),
        shell_.t(jbool(v, "crash_reports", true) && level != "off" ? "telemetry.level.on" : "telemetry.level.off"));

    // Crash Reports: each one is listed here for an hour before it is sent, and can be deleted.
    Node* cc = card("telemetry-crashes", shell_.t("telemetry.crashes.title"));
    const json* crashes = v.contains("crashes") && v["crashes"].is_array() ? &v["crashes"] : nullptr;
    if (!crashes || crashes->empty()) {
        text(*cc, shell_.t("telemetry.crashes.empty"), Paint::text_secondary, "telemetry-crashes-empty");
    } else {
        text(*cc, shell_.t("telemetry.crashes.intro"), Paint::text_secondary);
        auto* list = cc->emplace<Node>("telemetry-crashes-list");
        list->role = ui::Role::list;
        list->name = shell_.t("telemetry.crashes.title");
        list->gap = 10;
        for (const json& cr : *crashes) {
            const std::string id = jstr(cr, "id");
            const std::string state = jstr(cr, "state");
            const json* sp = jobj(cr, "summary");
            const json& sum = sp ? *sp : v;
            const std::u16string when = shell_.clock(jint(cr, "at") / 1000);
            const std::u16string st = state == "held" ? shell_.t("telemetry.crashes.held", {{"time", shell_.clock(jint(cr, "send_after") / 1000)}})
                                                      : shell_.t("telemetry.crashes.states." + state);
            auto* item = list->emplace<Node>("telemetry-crash-" + id);
            item->role = ui::Role::list_item;
            item->gap = 2;
            item->name = u16(jstr(cr, "process")) + u", " + when + u", " + st;
            auto* head = item->emplace<Node>();
            head->axis = ui::Axis::row;
            head->gap = 8;
            head->emplace<ui::Label>(shell_.ui(), u16(jstr(cr, "process")), TextStyle::body_strong, Paint::text, false)->role = ui::Role::none;
            head->emplace<ui::Label>(shell_.ui(), when, TextStyle::caption, Paint::text_secondary, false)->role = ui::Role::none;
            head->emplace<ui::Badge>(shell_.ui(), st, state == "sent" ? Paint::surface_alt : Paint::warning_background, Paint::text)->role = ui::Role::none;
            const char d = shell_.decimal();
            std::u16string detail = shell_.t("telemetry.crashes.detail", {{"code", u16(jstr(sum, "exception_code"))},
                                                                          {"module", u16(jstr(sum, "module"))},
                                                                          {"size", u16(format_bytes(static_cast<double>(jint(cr, "size")), d))}});
            item->emplace<ui::Label>(shell_.ui(), detail, TextStyle::caption, Paint::text_secondary, true, "telemetry-crash-detail-" + id)->role = ui::Role::none;
            std::string modules;
            if (sum.contains("modules") && sum["modules"].is_array())
                for (const json& m : sum["modules"])
                    if (m.is_string()) modules += (modules.empty() ? "" : ", ") + m.get<std::string>();
            if (!modules.empty())
                item->emplace<ui::Label>(shell_.ui(), shell_.t("telemetry.crashes.modules", {{"threads", u16(std::to_string(jint(sum, "threads")))},
                                                                                              {"modules", u16(modules)}}),
                                         TextStyle::caption, Paint::text_secondary, true)
                    ->role = ui::Role::none;
            if (state != "sent")
                button(*item, "telemetry-crash-delete-" + id, shell_.t("telemetry.crashes.delete"), [this, id] {
                    act("crash", "telemetry.crash.delete", {{"id", id}}, [this](const json& r) {
                        telemetry_ = r;
                        shell_.announce(shell_.t("telemetry.crashes.deleted"));
                    });
                })->enabled = busy_.empty();
        }
        error_line(*cc, "crash");
    }

    Node* rc = card("telemetry-records", shell_.t("telemetry.records.title"));    text(*rc, shell_.t("telemetry.records.intro"), Paint::text_secondary);
    const json* recs = v.contains("records") && v["records"].is_array() ? &v["records"] : nullptr;
    if (!recs || recs->empty()) {
        text(*rc, shell_.t("telemetry.records.empty"), Paint::text_secondary, "telemetry-records-empty");
        return;
    }
    const size_t total = recs->size(), shown = std::min(total, telemetry_shown_);
    text(*rc, shell_.t("telemetry.records.showing", {{"shown", u16(std::to_string(shown))}, {"total", u16(std::to_string(total))}}),
         Paint::text_secondary, "telemetry-records-count");
    auto* list = rc->emplace<Node>("telemetry-records-list");
    list->role = ui::Role::list;
    list->name = shell_.t("telemetry.records.title");
    list->gap = 10;
    for (size_t i = 0; i < shown; ++i) {
        const json& r = (*recs)[i];
        const bool sent = jstr(r, "state") == "sent";
        const std::u16string when = shell_.clock(jint(r, "time") / 1000);
        const std::u16string state = shell_.t(sent ? "telemetry.records.sent" : "telemetry.records.buffered");
        const std::u16string event = u16(jstr(r, "event"));
        auto* item = list->emplace<Node>("telemetry-record-" + std::to_string(jint(r, "id")));
        item->role = ui::Role::list_item;
        item->gap = 2;
        item->name = event + u", " + state + u", " + when;
        auto* head = item->emplace<Node>();
        head->axis = ui::Axis::row;
        head->gap = 8;
        head->emplace<ui::Label>(shell_.ui(), event, TextStyle::body_strong, Paint::text, false)->role = ui::Role::none;
        head->emplace<ui::Label>(shell_.ui(), when, TextStyle::caption, Paint::text_secondary, false)->role = ui::Role::none;
        head->emplace<ui::Badge>(shell_.ui(), state, sent ? Paint::surface_alt : Paint::info_background, Paint::text)->role = ui::Role::none;
        if (const json* a = jobj(r, "attrs"))
            for (const auto& [k, val] : a->items())
                item->emplace<ui::Label>(shell_.ui(), u16(k) + u": " + u16(val.is_string() ? val.get<std::string>() : val.dump()),
                                         TextStyle::caption, Paint::text_secondary, true)
                    ->role = ui::Role::none;
    }
    if (shown < total)
        button(*rc, "telemetry-more", shell_.t("telemetry.records.more"), [this] {
            telemetry_shown_ += 100;
            rebuild();
        });
}

// ---- About ---------------------------------------------------------------------------------------------------------

void PagePane::build_about() {
    Node* c = card("version", shell_.t("about.product"));
    if (!version_error_.empty()) {
        text(*c, version_error_, Paint::error, "about-error");
        return;
    }
    if (!version_.is_object()) {
        text(*c, shell_.t("common.loading"), Paint::text_secondary, "about-loading");
        return;
    }
    auto field = [&](const char* k) {
        const json* x = version_.contains(k) ? &version_[k] : nullptr;
        if (!x || x->is_null()) return std::u16string();
        return u16(x->is_string() ? x->get<std::string>() : x->dump());
    };
    row(*c, "about-version", shell_.t("about.version"), field("version"));
    row(*c, "about-channel", shell_.t("about.channel"), field("channel"));
    row(*c, "about-commit", shell_.t("about.commit"), field("commit"));
    row(*c, "about-pid", shell_.t("about.pid"), field("pid"));
    auto* actions = c->emplace<Node>();
    actions->axis = ui::Axis::row;
    actions->gap = 12;
    button(*actions, "about-copy", shell_.t("about.copy"), [this] {
        const bool ok = shell_.copy_text(about_text(version_));
        copied_ = shell_.t(ok ? "about.copied" : "about.copy_failed");
        shell_.announce(copied_);
        rebuild();
    });
    if (!copied_.empty()) {
        auto* l = actions->emplace<ui::Label>(shell_.ui(), copied_, TextStyle::body, Paint::text_secondary, false, "about-copy-state");
        l->padding.top = 6;
    }
    // client/desktop-host Third-Party Notices: the core's components, then the speech-to-text package's.
    if (!version_.contains("notices") || !version_["notices"].is_array()) return;
    Node* n = card("notices", shell_.t("about.notices"));
    for (const char* package : {"core", "speech-to-text"}) {
        auto* h = n->emplace<ui::Label>(shell_.ui(), shell_.t(std::string("about.package.") + package), TextStyle::body_strong,
                                        Paint::text, true, std::string("about-package-") + package);
        h->role = ui::Role::heading;
        int i = 0;
        for (const json& e : version_["notices"]) {
            if (jstr(e, "package") != package) continue;
            const std::string id = std::string("about-notice-") + package + "-" + std::to_string(i++);
            std::string line = jstr(e, "name");
            if (!jstr(e, "version").empty()) line += " " + jstr(e, "version");
            row(*n, id, u16(line), u16(jstr(e, "licence")));
            if (!jstr(e, "attribution").empty())
                text(*n, u16(jstr(e, "attribution")), Paint::text_secondary, id + "-attribution");
        }
    }
}

}  // namespace wb::shell
