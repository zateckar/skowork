// The frame of the window: navigation, banners and the status bar.
#include <algorithm>

#include "shell/derive.h"
#include "shell/format.h"
#include "shell/shell.h"
#include "ui/text16.h"

namespace wb::shell {

using ui::Node;
using ui::Paint;
using ui::Sizing;
using ui::TextStyle;

namespace {

namespace glyph {
constexpr char16_t inbox = 0xE715, unread = 0xE8BD, action = 0xE7BA, running = 0xE768, all = 0xE8FD, archive = 0xE7B8,
                   status = 0xE9D9, settings = 0xE713, about = 0xE946, folder = 0xE8B7, add = 0xE710, knowledge = 0xE8F1, apps = 0xE71D,
                   chats = 0xE8F2;
}

// One entry of the navigation: an icon, a label (hidden when compact), a count; selected for the current place.
class NavItem : public Node {
public:
    NavItem(ui::Ui& ui, std::string id, char16_t g, std::u16string label, long long count, bool selected, bool compact,
            std::function<void()> go)
        : Node(std::move(id)), ui_(ui), glyph_(g), label_(std::move(label)), selected_(selected), compact_(compact),
          go_(std::move(go)) {
        role = ui::Role::list_item;
        focusable = true;
        name = label_;
        if (count > 0) {
            count_ = ui::to_utf16(std::to_string(count));
            name += u" (" + count_ + u")";
        }
        if (compact_) help = name;
        height = Sizing::fixed;
        fixed_h = 36;
    }
    bool can_invoke() const override { return true; }
    void invoke() override {
        if (go_) go_();
    }
    int selected_state() const override { return selected_ ? 1 : 0; }
    void select() override { invoke(); }
    int cursor() const override { return 1; }
    void draw(ui::Canvas& c) override {
        const ui::Rect f = frame().inset(2);
        if (selected_) c.round(f, 4, Paint::selection);
        else if (pressed_) c.round(f, 4, Paint::text, Paint::none, 0.10f);
        else if (hovered_) c.round(f, 4, Paint::text, Paint::none, 0.05f);
        if (selected_ && !ui_.theme.high_contrast) c.round({f.x, f.y + 9, 3, f.h - 18}, 1.5f, Paint::accent);
        const Paint ink = selected_ && ui_.theme.high_contrast ? Paint::selection_text : Paint::text;
        IDWriteTextLayout* g = ui_.layout(std::u16string(1, glyph_), TextStyle::icon, 100.f, false);
        DWRITE_TEXT_METRICS gm{};
        if (g) g->GetMetrics(&gm);
        const float cy = f.y + f.h / 2;
        const float gx = compact_ ? f.x + (f.w - gm.width) / 2 : f.x + 12;
        c.text(g, gx, cy - gm.height / 2, selected_ && !ui_.theme.high_contrast ? Paint::accent : ink);  // brand accent on the current place
        if (!count_.empty()) {
            IDWriteTextLayout* n = ui_.layout(count_, TextStyle::caption, 100.f, false);
            DWRITE_TEXT_METRICS nm{};
            if (n) n->GetMetrics(&nm);
            // Counts are accent badges (improve-desktop-look: Brand Colours).
            const float pw = std::max(nm.width + 12, nm.height + 2);
            const ui::Rect pill = compact_ ? ui::Rect{f.right() - pw - 2, f.y + 2, pw, nm.height + 2}
                                           : ui::Rect{f.right() - pw - 8, cy - (nm.height + 2) / 2, pw, nm.height + 2};
            c.round(pill, pill.h / 2, Paint::accent, ui_.theme.high_contrast ? Paint::text : Paint::none);
            c.text(n, pill.x + (pw - nm.width) / 2, pill.y + 1, Paint::accent_text);
            count_w_ = pw + 14;
        }
        if (!compact_) {
            const float x = f.x + 12 + 16 + 12;
            IDWriteTextLayout* l = ui_.layout(label_, TextStyle::body, std::max(1.f, f.right() - x - count_w_), false);
            DWRITE_TEXT_METRICS lm{};
            if (l) l->GetMetrics(&lm);
            c.text(l, x, cy - lm.height / 2, ink);
        }
    }

private:
    ui::Ui& ui_;
    char16_t glyph_;
    std::u16string label_, count_;
    bool selected_, compact_;
    float count_w_ = 0;
    std::function<void()> go_;
};

Node* section(Node& parent, std::string id, std::u16string name) {
    auto* s = parent.emplace<Node>(std::move(id));
    s->role = ui::Role::list;
    s->name = std::move(name);
    return s;
}

}  // namespace

// ---- navigation ----------------------------------------------------------------------------------------------------

NavPane::NavPane(Shell& shell) : Box("nav", Paint::surface_alt), shell_(shell) {
    role = ui::Role::pane;
    width = Sizing::fixed;
    fixed_w = kNavWide;
    height = Sizing::fill;
    padding = {6, 0, 6, 8};  // the brand row is the top of the caption band
    gap = 4;
}

void NavPane::set_compact(bool compact) {
    if (compact == compact_ && !children().empty()) return;
    compact_ = compact;
    fixed_w = compact ? kNavCompact : kNavWide;
    rebuild();
}

void NavPane::rebuild() {
    name = shell_.t("nav.label");
    clear_children();
    const Route& r = shell_.route();
    const bool inbox_route = r.name == RouteName::inbox || r.name == RouteName::thread || r.name == RouteName::chat;

    // The Workbench mark and name in the caption band, as File Explorer's tabs (improve-window-frame D3): the window moves
    // from here.
    auto* brand = emplace<Node>("nav-brand");
    brand->axis = ui::Axis::row;
    brand->height = Sizing::fixed;
    brand->fixed_h = ui::WindowButtons::kHeight;
    brand->padding.left = compact_ ? 9.f : 10.f;
    brand->gap = 10;
    brand->emplace<ui::BrandMark>(shell_.ui(), 18.f);
    if (!compact_) {
        auto* n = brand->emplace<ui::Label>(shell_.ui(), shell_.t("about.product"), TextStyle::caption, Paint::text, false, "nav-brand-name");
        n->role = ui::Role::none;  // the window's name carries it
        n->padding.top = 12;
    }

    auto* top = emplace<Node>("nav-top");
    top->axis = compact_ ? ui::Axis::column : ui::Axis::row;
    top->gap = 4;
    top->padding = ui::Insets::xy(compact_ ? 0 : 4, 4);
    auto* nt = compact_ ? static_cast<ui::Button*>(top->emplace<ui::IconButton>(shell_.ui(), "nav-new-thread", glyph::add,
                                                                                shell_.t("nav.new_thread"),
                                                                                ui::Button::Kind::primary))
                        : top->emplace<ui::Button>(shell_.ui(), "nav-new-thread", shell_.t("nav.new_thread"),
                                                   ui::Button::Kind::primary, glyph::add);
    if (!compact_) nt->width = Sizing::fill;
    nt->on_click = [this] { shell_.navigate("#/threads/new"); };
    // New Chat next to New thread while messaging is on (fold-chats-into-inbox D2).
    const json* msg = jobj(shell_.status(), "messaging");
    if (msg && jbool(*msg, "available") && jbool(*msg, "enabled")) {
        auto* nc = top->emplace<ui::IconButton>(shell_.ui(), "nav-new-chat", glyph::chats, shell_.t("nav.new_chat"));
        nc->on_click = [this] { shell_.ask_new_chat(); };
    }

    const Counts& n = shell_.counts();
    struct V {
        const char* view;
        char16_t g;
        long long count;
    };
    const V views[] = {{"inbox", glyph::inbox, 0},      {"unread", glyph::unread, n.unread}, {"action", glyph::action, n.action},
                       {"running", glyph::running, n.running}, {"all", glyph::all, 0},   {"archive", glyph::archive, 0}};
    Node* vs = section(*this, "nav-views", shell_.t("nav.views"));
    for (const V& v : views) {
        const std::string view = v.view;
        vs->emplace<NavItem>(shell_.ui(), "nav-" + view, v.g, shell_.t("views." + view), v.count,
                             inbox_route && shell_.view() == view, compact_,
                             [this, view] { shell_.navigate(view_hash(view)); });
    }

    auto* fh = emplace<Node>("nav-folders-header");
    fh->axis = ui::Axis::row;
    fh->padding = {compact_ ? 0.f : 12.f, 8, 0, 0};
    if (!compact_) {
        auto* l = fh->emplace<ui::Label>(shell_.ui(), shell_.t("nav.folders"), TextStyle::caption, Paint::text_secondary, true);
        l->padding.top = 8;
        l->role = ui::Role::heading;
    }
    auto* nf = fh->emplace<ui::IconButton>(shell_.ui(), "nav-new-folder", glyph::add, shell_.t("nav.new_folder"));
    nf->on_click = [this] { shell_.ask_new_folder(); };
    Node* fs = section(*this, "nav-folder-list", shell_.t("nav.folders"));
    for (const Folder& f : shell_.folders()) {
        const std::string view = "folder:" + f.id;
        fs->emplace<NavItem>(shell_.ui(), "nav-folder-" + f.id, glyph::folder, ui::to_utf16(f.name), 0,
                             inbox_route && shell_.view() == view, compact_,
                             [this, view] { shell_.navigate(view_hash(view)); });
    }

    auto* spacer = emplace<Node>("nav-spacer");
    spacer->height = Sizing::fill;
    Node* pages = section(*this, "nav-pages", shell_.t("nav.label"));
    struct P {
        const char* key;
        const char* hash;
        char16_t g;
        RouteName route;
    };
    const P ps[] = {{"knowledge", "#/knowledge", glyph::knowledge, RouteName::knowledge},
                    {"apps", "#/apps", glyph::apps, RouteName::apps},
                    {"status", "#/status", glyph::status, RouteName::status},
                    {"settings", "#/settings", glyph::settings, RouteName::settings},
                    {"about", "#/about", glyph::about, RouteName::about}};
    for (const P& p : ps) {
        const std::string hash = p.hash;
        pages->emplace<NavItem>(shell_.ui(), std::string("nav-") + p.key, p.g, shell_.t(std::string("nav.") + p.key), 0,
                                r.name == p.route || (p.route == RouteName::settings && r.name == RouteName::telemetry), compact_, [this, hash] { shell_.navigate(hash); });
    }
    relayout();
    invalidate();
}

// ---- banners -------------------------------------------------------------------------------------------------------

namespace {

class Banner : public ui::Box {
public:
    Banner(ui::Ui& ui, std::string id, const std::string& severity)
        : Box(std::move(id),
              severity == "error"     ? Paint::error_background
              : severity == "warning" ? Paint::warning_background
                                      : Paint::info_background,
              6, ui.theme.high_contrast ? Paint::text : Paint::none),
          ui_(ui), severity_(severity) {
        role = ui::Role::group;
        axis = ui::Axis::row;
        padding = {40, 10, 12, 10};
        gap = 12;
    }
    void draw(ui::Canvas& c) override {
        Box::draw(c);
        const char16_t g = severity_ == "error" ? ui::icon::error : severity_ == "warning" ? ui::icon::warning : ui::icon::info;
        const Paint p = severity_ == "error" ? Paint::error : severity_ == "warning" ? Paint::warning : Paint::info;
        c.text(ui_.layout(std::u16string(1, g), TextStyle::icon, 100.f, false), frame().x + 14, frame().y + 13, p);
    }

private:
    ui::Ui& ui_;
    std::string severity_;
};

}  // namespace

BannerArea::BannerArea(Shell& shell) : Node("banners"), shell_(shell) {
    role = ui::Role::group;
    padding = ui::Insets::xy(12, 0);
    gap = 8;
}

void BannerArea::rebuild() {
    name = shell_.t("banner.region");
    clear_children();
    const json& s = shell_.status();
    if (!is_app_status(s)) {
        padding.top = padding.bottom = 0;
        relayout();
        return;
    }
    const json* cp = jobj(s, "control_plane");
    const json* st = jobj(s, "storage");
    const std::vector<BannerSpec> specs = banners(s);
    padding.top = padding.bottom = specs.empty() ? 0.f : 8.f;
    for (const BannerSpec& b : specs) {
        if (std::find(dismissed_.begin(), dismissed_.end(), b.id) != dismissed_.end()) continue;
        const std::string base = "banner." + b.id;
        std::string detail;
        std::string param = "detail";
        if (b.id == "version_blocked") {
            detail = cp ? jstr(*cp, "min_version") : "";
            param = "min";
        } else if (b.id == "not_configured") {
            detail = cp ? jstr(*cp, "config_error") : "";
        } else if (b.id.starts_with("storage_")) {
            detail = st ? jstr(*st, "error") : "";
        }
        std::u16string body;
        if (shell_.ui().strings.has(base + ".body_none") && detail.empty()) body = shell_.t(base + ".body_none");
        else body = shell_.t(base + ".body", {{param, ui::to_utf16(detail)}});
        auto* banner = emplace<Banner>(shell_.ui(), "banner-" + b.id, b.severity);
        banner->name = shell_.t(base + ".title");
        auto* text = banner->emplace<Node>();
        text->gap = 2;
        text->emplace<ui::Label>(shell_.ui(), shell_.t(base + ".title"), TextStyle::body_strong);
        text->emplace<ui::Label>(shell_.ui(), body, TextStyle::body);
        if (b.id == "sign_in_required") {
            auto* a = banner->emplace<ui::Button>(shell_.ui(), "banner-sign-in", shell_.t(base + ".action"));
            a->on_click = [this] { shell_.call("identity.sign_in", json::object()); };
        } else if (b.severity == "error" || b.id.starts_with("storage_") || b.id == "not_configured") {
            auto* a = banner->emplace<ui::Button>(shell_.ui(), "banner-see-status", shell_.t("banner.see_status"));
            a->on_click = [this] { shell_.navigate("#/status"); };
        }
        if (b.severity != "error") {
            auto* x = banner->emplace<ui::IconButton>(shell_.ui(), "banner-dismiss-" + b.id, ui::icon::cancel,
                                                      shell_.t("banner.dismiss"));
            x->on_click = [this, id = b.id] {
                dismissed_.push_back(id);
                rebuild();
            };
        }
    }
    relayout();
    invalidate();
}

// ---- status bar ----------------------------------------------------------------------------------------------------

namespace {

// The context-usage ring with its label: a track circle and the used share as an arc.
class ContextMeter : public Node {
public:
    ContextMeter(double fraction, bool warn) : Node("status-context"), fraction_(fraction), warn_(warn) {
        role = ui::Role::text;
        axis = ui::Axis::row;
        padding.left = 22;
    }
    void draw(ui::Canvas& c) override {
        const ui::Rect f = frame();
        const float r = 7, cx = f.x + 9, cy = f.y + f.h / 2;
        c.arc(cx, cy, r, 2.f, Paint::border, 1.0);
        c.arc(cx, cy, r, 2.f, warn_ ? Paint::warning : Paint::accent, fraction_);
    }

private:
    double fraction_;
    bool warn_;
};

}  // namespace

namespace {

// A small round state mark before a status line (connected or not).
class StatusDot : public Node {
public:
    explicit StatusDot(Paint p) : paint_(p) {
        width = height = Sizing::fixed;
        fixed_w = 8;
        fixed_h = 16;
    }
    void draw(ui::Canvas& c) override {
        const ui::Rect f = frame();
        c.round({f.x, f.y + (f.h - 8) / 2, 8, 8}, 4, paint_);
    }

private:
    Paint paint_;
};

}  // namespace

StatusBarView::StatusBarView(Shell& shell) : Box("status-bar", Paint::surface_alt), shell_(shell) {
    role = ui::Role::status_bar;
    axis = ui::Axis::row;
    padding = ui::Insets::xy(12, 4);
    gap = 16;
}

void StatusBarView::rebuild() {
    name = shell_.t("status_bar.label");
    clear_children();
    const json& s = shell_.status();
    auto label = [&](std::u16string text, std::string id, Paint p = Paint::text_secondary) {
        return emplace<ui::Label>(shell_.ui(), std::move(text), TextStyle::caption, p, false, std::move(id));
    };
    if (!is_app_status(s)) {
        label(shell_.t("status_bar.unknown"), "status-connection");
    } else {
        const std::string kind = connection_kind(s);
        // Offline with a known start: "Offline since 14:05" (add-offline-operation D4).
        const json* cpj = jobj(s, "control_plane");
        const long long since = cpj ? jint(*cpj, "offline_since", 0) : 0;
        auto* conn = emplace<Node>();
        conn->axis = ui::Axis::row;
        conn->width = Sizing::fit;
        conn->gap = 6;
        conn->emplace<StatusDot>(kind == "connected" ? Paint::success : Paint::warning);
        conn->emplace<ui::Label>(shell_.ui(),
                                 kind == "disconnected" && since > 0 && shell_.offline()
                                     ? shell_.t("status_bar.offline_since", {{"since", shell_.moment(since)}})
                                     : shell_.t("status.connection." + kind),
                                 TextStyle::caption, kind == "connected" ? Paint::text_secondary : Paint::warning, false, "status-connection");
        // The user's own Presence while messaging is on; a click switches busy (fold-chats-into-inbox D3).
        if (const json* msg = jobj(s, "messaging"); msg && jbool(*msg, "available") && jbool(*msg, "enabled")) {
            const bool busy = jbool(*msg, "busy");
            auto* pres = emplace<Node>("status-presence-row");
            pres->axis = ui::Axis::row;
            pres->width = Sizing::fit;
            pres->gap = 6;
            pres->emplace<StatusDot>(busy ? Paint::error : Paint::success);
            auto* b = pres->emplace<ui::Button>(shell_.ui(), "status-presence", shell_.t(busy ? "status_bar.busy" : "status_bar.available"),
                                                ui::Button::Kind::subtle);
            b->make_inline();
            b->name = shell_.t(busy ? "status_bar.busy_name" : "status_bar.available_name");
            b->help = shell_.t(busy ? "status_bar.busy_title" : "status_bar.available_title");
            b->on_click = [this, busy] { shell_.set_busy(!busy); };
        }
        // Persistent while content-level telemetry is in force (client/telemetry Content Level).
        if (const json* tel = jobj(s, "telemetry"); tel && jbool(*tel, "content_active")) {
            auto* b = emplace<ui::Button>(shell_.ui(), "status-content-telemetry", shell_.t("status_bar.content_telemetry"), ui::Button::Kind::subtle);
            b->make_inline();
            b->help = shell_.t("status_bar.content_telemetry_title");
            b->on_click = [this] { shell_.navigate("#/settings/telemetry"); };
        }
        const McpHealth h = mcp_health(s);
        if (h.total > 0) {
            std::u16string text = !h.failed.empty()
                                      ? shell_.t("status_bar.connectors_failed", {{"names", ui::to_utf16([&] {
                                                                               std::string names;
                                                                               for (const auto& f : h.failed)
                                                                                   names += (names.empty() ? "" : ", ") + f;
                                                                               return names;
                                                                           }())}})
                                      : shell_.t("status_bar.connectors", {{"healthy", ui::to_utf16(std::to_string(h.healthy))},
                                                                    {"total", ui::to_utf16(std::to_string(h.total))}});
            auto* b = emplace<ui::Button>(shell_.ui(), "status-mcp", std::move(text), ui::Button::Kind::subtle);
            b->make_inline();
            b->help = shell_.t("status_bar.connectors_title");
            b->on_click = [this] { shell_.navigate("#/settings"); };
        }
        if (const json* p = jobj(s, "policy")) {
            const std::string state = jstr(*p, "state");
            // The version is a number in the status (a string in older builds).
            const auto ver = p->find("version");
            const std::u16string v = ui::to_utf16(ver != p->end() && ver->is_number_integer() ? std::to_string(ver->get<long long>()) : jstr(*p, "version"));
            label(state == "valid"     ? shell_.t("status_bar.policy", {{"version", v}})
                  : state == "expired" ? shell_.t("status_bar.policy_expired", {{"version", v}})
                                       : shell_.t("status_bar.no_policy"),
                  "status-policy");
        }
    }
    // Context usage of the open Thread: warns at 75 % and offers Compact now.
    const json& ctx = shell_.context();
    if (!shell_.open_id().empty() && ctx.is_object()) {
        const double tokens = static_cast<double>(jint(ctx, "tokens")), window = static_cast<double>(jint(ctx, "window"));
        const double budget = static_cast<double>(jint(ctx, "input_budget"));
        const bool budget_known = ctx.contains("input_budget") && ctx["input_budget"].is_number_integer();
        const double capacity = budget_known ? budget : window;  // Older runtimes report only the model window.
        const double fraction = capacity > 0 ? std::min(1.0, tokens / capacity) : (budget_known ? 1.0 : 0.0);
        const std::u16string pct = ui::to_utf16(format_percent(fraction, shell_.decimal()));
        const bool warn = fraction >= 0.75;
        auto* meter = emplace<ContextMeter>(fraction, warn);
        const auto context_label = shell_.t(budget_known ? "status_bar.context_estimate" : "status_bar.context", {{"pct", pct}});
        const auto warning = shell_.t(budget_known ? "status_bar.context_budget_warn" : "status_bar.context_warn");
        meter->name = context_label + (warn ? u", " + warning : u"");
        const char* context_help = !budget_known ? "status_bar.context_title" :
            (jbool(ctx, "includes_tools") ? "status_bar.context_budget_tools_title" : "status_bar.context_budget_title");
        meter->help = shell_.t(context_help, {{"pct", pct}});
        meter->emplace<ui::Label>(shell_.ui(), context_label, TextStyle::caption,
                                  warn ? Paint::warning : Paint::text_secondary, false)
            ->role = ui::Role::none;
        if (const json* last = jobj(ctx, "last_compaction"); last && jstr(*last, "kind") == "automatic")
            label(shell_.t("status_bar.auto_compacted"), "status-auto-compacted");
        if (warn) {
            auto* b = emplace<ui::Button>(shell_.ui(), "status-compact", shell_.t("status_bar.compact_now"), ui::Button::Kind::subtle);
            b->make_inline();
            b->help = warning;
            b->on_click = [this, id = shell_.open_id()] { shell_.compact(id); };
        }
    }
    auto* spacer = emplace<Node>();
    spacer->width = Sizing::fill;
    if (is_app_status(s))
        if (const json* v = jobj(s, "version"))
            label(shell_.t("about.product") + u" " + ui::to_utf16(jstr(*v, "version")), "status-version");
    relayout();
    invalidate();
}

}  // namespace wb::shell
