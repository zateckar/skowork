// The open Thread and the Activity rail.
#include <algorithm>

#include "shell/format.h"
#include "shell/shell.h"
#include "ui/text16.h"

namespace wb::shell {

using ui::Node;
using ui::Paint;
using ui::Sizing;
using ui::TextStyle;

namespace {
constexpr char16_t kUnreadGlyph = 0xE8BD, kRailGlyph = 0xE8A0, kMoreGlyph = 0xE712;
}

ThreadPane::ThreadPane(Shell& shell) : Box("thread-pane", Paint::background), shell_(shell) {
    role = ui::Role::pane;
    height = Sizing::fill;
}

void ThreadPane::rebuild() {
    // The feed and the composer stay while their Thread stays open; the composer keeps the keyboard focus.
    ui::Tree* tr = Node::tree();
    Node* focus = tr ? tr->focused() : nullptr;
    bool composer_focus = false;
    for (Node* n = focus; n && composer_; n = n->parent())
        if (n == composer_) composer_focus = true;
    if (feed_) kept_feed_.reset(static_cast<FeedView*>(remove(feed_).release()));
    if (composer_) kept_composer_.reset(static_cast<ComposerView*>(remove(composer_).release()));
    feed_ = nullptr;
    composer_ = nullptr;
    banner_ = nullptr;
    clear_children();
    auto add_composer = [&](const std::string& key) {
        std::unique_ptr<ComposerView> c = std::move(kept_composer_);
        const bool kept = c && c->thread_key() == key;
        if (!kept) c = std::make_unique<ComposerView>(shell_, key);
        composer_ = static_cast<ComposerView*>(add(std::move(c)));
        if (kept && composer_focus && tr) tr->set_focus(focus);
        composer_->update();
    };
    const Route& r = shell_.route();
    if (r.name == RouteName::new_thread) {
        name = shell_.t("composer.new_title");
        padding = ui::Insets::all(24);
        gap = 8;
        auto* h = emplace<ui::Label>(shell_.ui(), shell_.t("composer.new_title"), TextStyle::title, Paint::text, true, "new-thread-title");
        h->role = ui::Role::heading;
        emplace<ui::Label>(shell_.ui(), shell_.t("composer.new_hint"), TextStyle::body, Paint::text_secondary, true, "new-thread-hint");
        add_composer("new");
        composer_->on_sent = [this](const std::string& thread_id) { shell_.navigate(thread_hash(thread_id)); };
        relayout();
        invalidate();
        return;
    }
    if (shell_.open_id().empty()) {
        name = shell_.t("thread.select");
        padding = ui::Insets::all(32);
        gap = 8;
        auto* top = emplace<Node>();
        top->height = Sizing::fill;
        top->weight = 1;
        emplace<ui::Label>(shell_.ui(), shell_.t("thread.select"), TextStyle::subtitle, Paint::text_secondary, true, "thread-hint");
        emplace<ui::Label>(shell_.ui(), shell_.t("thread.select_hint"), TextStyle::body, Paint::text_secondary, true);
        auto* bottom = emplace<Node>();
        bottom->height = Sizing::fill;
        bottom->weight = 2;
        relayout();
        invalidate();
        return;
    }
    const std::string id = shell_.open_id();
    if (!shell_.feed_error().empty()) {
        const bool missing = shell_.feed_error() == "not_found";
        name = shell_.t(missing ? "thread.not_found" : "thread.load_failed");
        padding = ui::Insets::all(32);
        gap = 12;
        auto* h = emplace<ui::Label>(shell_.ui(), name, TextStyle::subtitle, Paint::text, true, "thread-state");
        h->role = ui::Role::heading;
        if (missing) {
            auto* back = emplace<ui::Button>(shell_.ui(), "thread-state-back", shell_.t("thread.back"));
            back->width = Sizing::fit;
            back->on_click = [this] { shell_.navigate(view_hash(shell_.view())); };
        } else {
            emplace<ui::Label>(shell_.ui(), ui::to_utf16(shell_.feed_error()), TextStyle::body, Paint::error, true, "thread-state-error");
            auto* retry = emplace<ui::Button>(shell_.ui(), "thread-state-retry", shell_.t("common.retry"));
            retry->width = Sizing::fit;
            retry->on_click = [this] { shell_.load_feed(); };
        }
        relayout();
        invalidate();
        return;
    }
    padding = {};
    gap = 0;
    const ThreadEntry* e = shell_.entry(id);
    const std::u16string title = !e ? shell_.t("common.loading") : e->title.empty() ? shell_.t("list.untitled") : ui::to_utf16(e->title);
    name = title;

    auto* header = emplace<Node>("thread-header");
    header->axis = ui::Axis::row;
    header->padding = ui::Insets::xy(12, 8);
    header->gap = 4;
    if (shell_.single_pane()) {
        auto* back = header->emplace<ui::IconButton>(shell_.ui(), "thread-back", ui::icon::back, shell_.t("thread.back"));
        back->on_click = [this] { shell_.navigate(view_hash(shell_.view())); };
    }
    auto* h = header->emplace<ui::Label>(shell_.ui(), title, TextStyle::subtitle, Paint::text, true, "thread-title");
    h->role = ui::Role::heading;
    h->padding.top = 4;
    auto* tools = header->emplace<Node>("thread-toolbar");
    tools->role = ui::Role::group;
    tools->name = shell_.t("thread.toolbar");
    tools->axis = ui::Axis::row;
    tools->width = Sizing::fit;
    tools->gap = 2;
    if (e) {
        const bool archived = e->archived, pinned = e->pinned;
        auto* a = tools->emplace<ui::IconButton>(shell_.ui(), "thread-archive", ui::icon::archive,
                                                 shell_.t(archived ? "thread.restore" : "thread.archive"));
        a->on_click = [this, id, archived] { shell_.archive(id, !archived); };
        auto* u = tools->emplace<ui::IconButton>(shell_.ui(), "thread-mark-unread", kUnreadGlyph, shell_.t("thread.mark_unread"));
        u->on_click = [this, id] { shell_.mark_unread(id); };
        auto* p = tools->emplace<ui::IconButton>(shell_.ui(), "thread-pin", ui::icon::pin, shell_.t(pinned ? "thread.unpin" : "thread.pin"));
        p->on_click = [this, id, pinned] { shell_.pin(id, !pinned); };
        auto* more = tools->emplace<ui::IconButton>(shell_.ui(), "thread-more", kMoreGlyph, shell_.t("tooltip.more"));
        more->on_click = [this, id, more] { shell_.show_thread_menu(id, more->frame()); };
    }
    const bool rail_shown = shell_.rail_docked() ? shell_.rail_open() : false;
    auto* rail = tools->emplace<ui::IconButton>(shell_.ui(), "thread-rail", kRailGlyph,
                                                shell_.t(rail_shown ? "thread.hide_rail" : "thread.show_rail"));
    rail->on_click = [this] { shell_.toggle_rail(); };
    header->emplace<ui::CaptionSpace>();  // clear of the window buttons when this is the top-right pane

    // Origin Links: where a Thread that the user did not start came from.
    if (e && e->origin_kind != "user") {
        const json& ref = e->origin_ref;
        std::u16string text;
        std::u16string link_text;
        std::string link;  // a route ("#/...") or a URL
        auto s = [&ref](const char* k) { return jstr(ref, k); };
        if (e->origin_kind == "fork") {
            text = shell_.t("thread.origin.fork");
            if (!s("thread_id").empty()) {
                link = thread_hash(s("thread_id"), s("run_id").empty() ? std::nullopt
                                                                        : std::optional<MessageRef>(MessageRef{s("run_id"), jint(ref, "seq")}));
                link_text = shell_.t("thread.origin.open_source");
            }
        } else if (e->origin_kind == "trigger") {
            text = shell_.t("thread.origin.trigger", {{"name", ui::to_utf16(s("name").empty() ? s("trigger_id") : s("name"))},
                                                      {"at", ui::to_utf16(s("occurrence_at"))}});
            link = s("history_url");
            link_text = shell_.t("thread.origin.open_trigger");
        } else if (e->origin_kind == "remote") {
            text = shell_.t("thread.origin.remote", {{"issuer", ui::to_utf16(s("issuer"))}});
            link = s("link");
            link_text = shell_.t("thread.origin.open_remote");
        } else if (e->origin_kind == "meeting") {
            text = shell_.t("thread.origin.meeting", {{"title", ui::to_utf16(s("title"))}});
            link = s("link");
            link_text = shell_.t("thread.origin.open_meeting");
        } else if (!s("skill_test").empty()) {
            // A Skill test Thread (client/skills Skill Test Run): back to the editor of that Skill.
            text = shell_.t("thread.origin.skill_test", {{"name", ui::to_utf16(s("skill_test"))}});
            link = "#/skills/edit/" + s("skill_test");
            link_text = shell_.t("thread.origin.open_skill");
        } else {
            text = shell_.t("thread.origin.system");
        }
        auto* origin = emplace<Node>("thread-origin");
        origin->axis = ui::Axis::row;
        origin->gap = 8;
        origin->padding = {20, 0, 12, 6};
        origin->emplace<ui::Badge>(shell_.ui(), shell_.t(origin_badge_key(*e)), Paint::surface_alt, Paint::text_secondary);
        auto* l = origin->emplace<ui::Label>(shell_.ui(), text, TextStyle::caption, Paint::text_secondary, false, "thread-origin-text");
        l->padding.top = 1;
        if (!link.empty()) {
            auto* b = origin->emplace<ui::Button>(shell_.ui(), "thread-origin-link", link_text, ui::Button::Kind::subtle);
            b->width = Sizing::fit;
            b->role = ui::Role::link;
            ui::Ui* u = &shell_.ui();
            b->on_click = [this, u, link] {
                if (link.starts_with("#/")) shell_.navigate(link);
                else if (u->open_link) u->open_link(ui::to_utf16(link));  // asks first, shows the URL
            };
        }
    }

    banner_ = emplace<Node>("thread-banner");
    rebuild_banner();

    // The feed keeps its elements while the same Thread stays open.
    std::unique_ptr<FeedView> feed = std::move(kept_feed_);
    if (!feed || feed->thread_id() != id) feed = std::make_unique<FeedView>(shell_, id);
    feed_ = static_cast<FeedView*>(add(std::move(feed)));
    if (shell_.feed()) feed_->update();
    else if (feed_->children().empty()) feed_->emplace<ui::Label>(shell_.ui(), shell_.t("common.loading"), TextStyle::body, Paint::text_secondary);
    add_composer(id);
    relayout();
    invalidate();
}

void ThreadPane::update_feed() {
    if (!feed_ || !shell_.feed()) return;
    rebuild_banner();
    feed_->update();
    if (composer_) composer_->update();
}

void ThreadPane::update_composer() {
    if (composer_) composer_->update();
}

// Error Banners With Remedy: one remedy per model-access error code.
void ThreadPane::rebuild_banner() {
    if (!banner_) return;
    banner_->clear_children();
    banner_->padding = {};
    const auto& snap = shell_.feed();
    const auto failure = snap ? latest_error(*snap) : std::nullopt;
    if (!failure) {
        banner_->relayout();
        return;
    }
    const RunError& err = failure->error;
    std::string code = err.code;
    std::transform(code.begin(), code.end(), code.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const std::string base = "errors." + (shell_.ui().strings.has("errors." + code + ".title") ? code : std::string("other"));
    std::u16string body;
    if (code == "rate_limit" && err.retry_after_s)
        body = shell_.t(base + ".body", {{"seconds", ui::to_utf16(std::to_string(*err.retry_after_s))}});
    else if (code == "quota_exceeded" && !err.reset_at.empty())
        body = shell_.t(base + ".body", {{"at", ui::to_utf16(err.reset_at)}});
    else if (base == "errors.other" && !(err.reason.empty() && err.message.empty()))
        body = shell_.t(base + ".body", {{"reason", ui::to_utf16(err.reason.empty() ? err.message : err.reason)}});
    else if (shell_.ui().strings.has(base + ".body_none"))
        body = shell_.t(base + ".body_none");
    else
        body = shell_.t(base + ".body");

    if (announced_ != failure->run_id) {  // like an alert region: read out when it appears
        announced_ = failure->run_id;
        shell_.announce(shell_.t(base + ".title") + u". " + body);
    }
    auto* box = banner_->emplace<ui::Box>("error-banner", Paint::error_background, 8, shell_.ui().theme.high_contrast ? Paint::text : Paint::none);
    box->role = ui::Role::group;
    box->name = shell_.t(base + ".title");
    box->help = body;
    box->axis = ui::Axis::row;
    box->gap = 12;
    box->padding = ui::Insets::xy(14, 12);
    auto* text = box->emplace<Node>();
    text->gap = 2;
    text->emplace<ui::Label>(shell_.ui(), shell_.t(base + ".title"), TextStyle::body_strong, Paint::text, true, "error-banner-title");
    text->emplace<ui::Label>(shell_.ui(), body, TextStyle::body, Paint::text, true, "error-banner-body");
    auto* actions = box->emplace<Node>();
    actions->axis = ui::Axis::row;
    actions->width = Sizing::fit;
    actions->gap = 6;
    const std::string id = shell_.open_id();
    if (code == "auth_expired") {
        auto* b = actions->emplace<ui::Button>(shell_.ui(), "error-sign-in", shell_.t("errors.sign_in"), ui::Button::Kind::primary);
        b->on_click = [this] { shell_.sign_in(); };
    }
    if (code == "context_length") {
        // Fork from the last message before the failed request.
        std::string run_id;
        long long seq = 0;
        for (const RunInfo& r : snap->runs) {
            if (r.id == failure->run_id) break;
            for (const StoredEvent& ev : r.events)
                if (ev.kind == "message") {
                    run_id = r.id;
                    seq = ev.seq;
                }
        }
        if (run_id.empty())
            for (const RunInfo& r : snap->runs)
                if (r.id == failure->run_id)
                    for (const StoredEvent& ev : r.events)
                        if (ev.kind == "message" && run_id.empty()) {
                            run_id = r.id;
                            seq = ev.seq;
                        }
        if (!run_id.empty()) {
            auto* f = actions->emplace<ui::Button>(shell_.ui(), "error-fork", shell_.t("errors.fork"));
            f->on_click = [this, id, run_id, seq] { shell_.fork(id, run_id, seq); };
        }
        auto* c = actions->emplace<ui::Button>(shell_.ui(), "error-compact", shell_.t("errors.compact"));
        c->on_click = [this, id] { shell_.compact(id); };
    }
    if (code != "quota_exceeded") {
        auto* r = actions->emplace<ui::Button>(shell_.ui(), "error-retry", shell_.t("errors.retry"));
        r->on_click = [this, id] { shell_.retry(id); };
    }
    banner_->padding = {20, 4, 20, 8};
    banner_->relayout();
}

// ---- Activity rail -------------------------------------------------------------------------------------------------

RailPane::RailPane(Shell& shell, std::string id, bool drawer)
    : Box(std::move(id), Paint::surface_alt, drawer ? 0.f : 0.f, drawer ? Paint::border : Paint::none), shell_(shell), drawer_(drawer) {
    role = ui::Role::pane;
    width = Sizing::fixed;
    fixed_w = kRailWidth;
    height = Sizing::fill;
    padding = ui::Insets::all(12);
    gap = 8;
}

void RailPane::rebuild() {
    bodies_.clear();
    clear_children();
    name = shell_.t("rail.title");
    auto* head = emplace<Node>();
    head->axis = ui::Axis::row;
    auto* h = head->emplace<ui::Label>(shell_.ui(), shell_.t("rail.title"), TextStyle::body_strong, Paint::text, true);
    h->role = ui::Role::heading;
    h->padding.top = 6;
    if (drawer_) {
        auto* x = head->emplace<ui::IconButton>(shell_.ui(), automation_id() + "-close", ui::icon::cancel, shell_.t("rail.close"));
        x->on_click = [this] { shell_.toggle_rail(); };
    }
    head->emplace<ui::CaptionSpace>();
    const auto& feed = shell_.feed();
    const std::vector<RailGroup> groups = feed ? build_rail(*feed, shell_.now()) : std::vector<RailGroup>{};
    if (groups.empty()) {
        emplace<ui::Label>(shell_.ui(), shell_.t("rail.empty"), TextStyle::body, Paint::text_secondary);
    } else {
        auto* list = emplace<ui::ScrollView>(automation_id() + "-groups");
        list->scrolls = true;
        list->height = Sizing::fill;
        list->gap = 8;
        size_t gi = 0;
        for (const RailGroup& g : groups) {
            auto* card = list->emplace<ui::Card>(shell_.ui(), automation_id() + "-group-" + std::to_string(gi++),
                                                 shell_.count("rail.group", static_cast<long long>(g.entries.size())));
            card->body().gap = 6;
            for (const RailEntry& re : g.entries) {
                const std::string run = re.run_id;
                const std::string eid = automation_id() + "-entry-" + run;
                // A Skill's subagent is named by its Skill, so the delegation shows which Skill ran.
                const std::u16string title = re.skill.empty() ? ui::to_utf16(re.task)
                                                              : shell_.t("rail.skill", {{"skill", ui::to_utf16(re.skill)}, {"task", ui::to_utf16(re.task)}});
                auto* entry = card->body().emplace<ui::Card>(shell_.ui(), eid, title, true, expanded_.contains(run));
                entry->background = Paint::none;
                const std::u16string state = shell_.t("rail.states." + re.state);
                const std::u16string elapsed = ui::to_utf16(format_elapsed(re.duration_ms));
                entry->header().emplace<ui::Label>(shell_.ui(), state + u" · " + elapsed, TextStyle::caption, Paint::text_secondary,
                                                   false, eid + "-state");
                // The header's name is what a screen reader reads for the collapsed entry.
                entry->header().name = title + u", " + state + u", " + elapsed;
                entry->body().gap = 4;
                bodies_[run] = &entry->body();
                entry->on_expand = [this, run](bool on) {
                    if (!on) {
                        expanded_.erase(run);
                        return;
                    }
                    expanded_.insert(run);
                    if (!transcripts_.contains(run)) load(run);
                    else fill(run);
                };
                if (expanded_.contains(run)) fill(run);
            }
        }
    }
    relayout();
    invalidate();
}

// Child Run transcripts load on first expansion only (lazily) and are kept for the life of the pane.
void RailPane::load(const std::string& run_id) {
    transcripts_[run_id] = Transcript{};
    fill(run_id);
    std::weak_ptr<int> alive = alive_;
    shell_.call("runs.transcript", {{"run_id", run_id}}, [this, alive, run_id](Result<json> r) {
        if (!alive.lock()) return;
        Transcript& t = transcripts_[run_id];
        t.loaded = true;
        if (!r) {
            t.error = ui::to_utf16(r.error().message);
        } else if (const json* evs = r->contains("events") ? &(*r)["events"] : nullptr; evs && evs->is_array()) {
            for (const json& e : *evs) {
                if (jstr(e, "kind") != "message") continue;
                const json* p = jobj(e, "payload");
                const std::string text = p ? jstr(*p, "content") : std::string();
                if (!text.empty()) t.lines.push_back(ui::to_utf16(text));
            }
        }
        fill(run_id);
    });
}

void RailPane::fill(const std::string& run_id) {
    const auto b = bodies_.find(run_id);
    const auto t = transcripts_.find(run_id);
    if (b == bodies_.end() || t == transcripts_.end()) return;
    Node& body = *b->second;
    body.clear_children();
    const std::string eid = automation_id() + "-entry-" + run_id;
    ui::Ui& u = shell_.ui();
    if (!t->second.loaded) {
        body.emplace<ui::Label>(u, shell_.t("common.loading"), TextStyle::caption, Paint::text_secondary, false, eid + "-loading");
    } else if (!t->second.error.empty()) {
        body.emplace<ui::Label>(u, shell_.t("common.error_prefix") + u" " + t->second.error, TextStyle::caption, Paint::error, true,
                                eid + "-error");
    } else if (t->second.lines.empty()) {
        body.emplace<ui::Label>(u, shell_.t("rail.no_transcript"), TextStyle::caption, Paint::text_secondary, true, eid + "-empty");
    } else {
        auto* list = body.emplace<Node>(eid + "-transcript");
        list->role = ui::Role::list;
        list->name = shell_.t("rail.transcript");
        list->gap = 4;
        size_t i = 0;
        for (const std::u16string& line : t->second.lines)
            list->emplace<ui::Label>(u, line, TextStyle::body, Paint::text, true, eid + "-line-" + std::to_string(i++))->role =
                ui::Role::list_item;
    }
    relayout();
    invalidate();
}

}  // namespace wb::shell
