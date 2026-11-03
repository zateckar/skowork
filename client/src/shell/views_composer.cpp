// The composer (client/work-inbox Composer, Queue Or Steer While Running).
#include <filesystem>
#include <fstream>

#include "base/encoding.h"
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

constexpr long long kMaxBytes = 25LL * 1024 * 1024;
constexpr size_t kMaxFiles = 10;
constexpr UINT kSaveDelayMs = 400;

// One skill in the `/` list. The list is driven from the message field (Up, Down, Enter), as a combo box's list is,
// so the options take no focus; a click chooses one too.
class SkillOption : public Node {
public:
    SkillOption(ui::Ui& ui, std::string id, std::u16string label, std::u16string description, bool selected,
                std::function<void()> choose)
        : Node(std::move(id)), ui_(ui), label_(std::move(label)), description_(std::move(description)),
          selected_(selected), choose_(std::move(choose)) {
        role = ui::Role::list_item;
        name = label_;
        help = description_;
        height = Sizing::fixed;
        fixed_h = 30;
    }
    int selected_state() const override { return selected_ ? 1 : 0; }
    bool can_invoke() const override { return true; }
    void invoke() override { choose(); }
    int cursor() const override { return 1; }
    bool on_pointer(const ui::PointerEvent& e) override {
        if (e.kind == ui::PointerEvent::down && e.button == 0) return true;
        if (e.kind == ui::PointerEvent::up && e.button == 0 && frame().contains(e.x, e.y)) {
            choose();
            return true;
        }
        return false;
    }
    void draw(ui::Canvas& c) override {
        const ui::Rect f = frame();
        if (selected_) c.round(f.inset(1), 4, Paint::selection, Paint::none, ui_.theme.high_contrast ? 1.f : 0.5f);
        else if (hovered_) c.round(f.inset(1), 4, Paint::text, Paint::none, 0.05f);
        const Paint ink = selected_ && ui_.theme.high_contrast ? Paint::selection_text : Paint::text;
        const Paint dim = selected_ && ui_.theme.high_contrast ? Paint::selection_text : Paint::text_secondary;
        IDWriteTextLayout* l = ui_.layout(label_, TextStyle::code, std::max(1.f, f.w - 16), false);
        DWRITE_TEXT_METRICS m{};
        if (l) l->GetMetrics(&m);
        c.text(l, f.x + 8, f.y + (f.h - m.height) / 2, ink);
        const float x = f.x + 8 + m.width + 12;
        if (!description_.empty() && f.right() - 8 > x) {
            IDWriteTextLayout* d = ui_.layout(description_, TextStyle::caption, f.right() - 8 - x, false);
            DWRITE_TEXT_METRICS dm{};
            if (d) d->GetMetrics(&dm);
            c.text(d, x, f.y + (f.h - dm.height) / 2, dim);
        }
    }

private:
    // Choosing rebuilds the list, this option included: the callback runs from a copy.
    void choose() {
        const std::function<void()> f = choose_;
        f();
    }
    ui::Ui& ui_;
    std::u16string label_, description_;
    bool selected_;
    std::function<void()> choose_;
};

bool blank(const std::u16string& s) {
    return s.find_first_not_of(u" \t\r\n") == std::u16string::npos;
}

}  // namespace

ComposerView::ComposerView(Shell& shell, std::string thread_key)
    : Box("composer", Paint::surface, 0, Paint::none), shell_(shell), key_(std::move(thread_key)) {
    ui::Ui& u = shell.ui();
    role = ui::Role::group;
    name = shell.t("composer.label");
    padding = {20, 10, 20, 14};
    gap = 8;
    queued_ = emplace<Node>("composer-queued");
    queued_->role = ui::Role::group;
    queued_->name = shell.t("composer.queued_title");
    queued_->gap = 4;
    queued_->visible = false;
    blocked_ = emplace<ui::Label>(u, u"", TextStyle::body, Paint::text, true, "composer-blocked");
    blocked_->visible = false;
    attachments_ = emplace<Node>("composer-attachments");
    attachments_->role = ui::Role::list;
    attachments_->name = shell.t("composer.attachments");
    attachments_->axis = ui::Axis::row;
    attachments_->gap = 6;
    attachments_->visible = false;
    skills_ = emplace<ui::Box>("composer-skills", Paint::surface_alt, 6, Paint::border);
    skills_->padding = ui::Insets::all(8);
    skills_->gap = 4;
    skills_->visible = false;
    ui::TextField::Options o;
    o.multiline = true;
    o.placeholder = shell.t(key_ == "new" ? "composer.new_placeholder" : "composer.placeholder");
    o.min_lines = 3;
    o.max_lines = 12;
    field_ = emplace<ui::TextField>(u, "composer-input", shell.t("composer.label"), o);
    field_->on_change = [this] { text_changed(); };
    field_->on_key_down = [this](const ui::KeyEvent& e) { return field_key(e); };
    field_->on_files = [this](const std::vector<std::wstring>& paths) { attach(paths); };
    bar_ = emplace<Node>("composer-bar");
    bar_->axis = ui::Axis::row;
    bar_->gap = 8;
    error_ = emplace<ui::Label>(u, u"", TextStyle::body, Paint::error, true, "composer-error");
    error_->visible = false;

    // Drafts survive restarts: they live in the host.
    std::weak_ptr<int> alive = alive_;
    shell_.call("drafts.get", {{"thread_key", key_}}, [this, alive](Result<json> r) {
        if (!alive.lock() || !r) return;
        if (!text_.empty() || !files_.empty()) return;  // the user started before the draft arrived
        text_ = ui::to_utf16(jstr(*r, "text"));
        field_->set_text(text_);
        if (const json* a = jobj(*r, "attachments"); a && a->is_array())
            for (const json& x : *a) files_.push_back({jstr(x, "id"), jstr(x, "name"), jint(x, "size")});
        rebuild_attachments();
        rebuild_skills();
        relayout();
        invalidate();
    });
    rebuild_bar();
}

ComposerView::~ComposerView() {
    // Leaving the Thread or closing the window: what is pending is saved now. Only the host is used here; the window
    // may be going away.
    save_now();
}

// ---- the draft -----------------------------------------------------------------------------------------------------

void ComposerView::text_changed() {
    const std::optional<std::string> before = skill_query();
    text_ = field_->text();
    dirty_ = true;
    save_soon();
    if (skill_query() != before) skill_index_ = 0;
    rebuild_skills();
    relayout();
}

void ComposerView::save_soon() {
    if (save_timer_) shell_.window().cancel_timer(save_timer_);
    std::weak_ptr<int> alive = alive_;
    save_timer_ = shell_.window().set_timer(kSaveDelayMs, [this, alive] {
        if (!alive.lock()) return;
        save_timer_ = 0;
        save_now();
    });
}

void ComposerView::save_now() {
    if (!dirty_) return;
    dirty_ = false;
    shell_.call("drafts.save", {{"thread_key", key_}, {"text", ui::to_utf8(text_)}});
}

// ---- keys ----------------------------------------------------------------------------------------------------------

bool ComposerView::field_key(const ui::KeyEvent& e) {
    // Escape discards this composer's recording (add-dictation D3).
    if (e.key == ui::kKeyEscape && !e.ctrl && !e.alt && !e.shift && shell_.cancel_dictation_of("thread:" + key_)) return true;
    const auto matches = skill_matches();
    if (!matches.empty() && !e.ctrl && !e.alt && (e.key == ui::kKeyDown || e.key == ui::kKeyUp)) {
        skill_index_ = (skill_index_ + (e.key == ui::kKeyDown ? 1 : matches.size() - 1)) % matches.size();
        rebuild_skills();
        return true;
    }
    if (e.key != ui::kKeyEnter || e.shift || e.alt) return false;  // Shift+Enter starts a new line
    if (!matches.empty() && !e.ctrl) {
        choose_skill(std::min(skill_index_, matches.size() - 1));
        return true;
    }
    const auto& snap = shell_.feed();
    const bool running = key_ != "new" && snap && snap->entry.id == key_ && running_run(*snap);
    send(e.ctrl && running);
    return true;
}

// ---- `/` skills ----------------------------------------------------------------------------------------------------

std::optional<std::string> ComposerView::skill_query() const {
    if (text_.empty() || text_[0] != u'/') return std::nullopt;
    if (text_.find_first_of(u" \t\r\n", 1) != std::u16string::npos) return std::nullopt;
    return ui::to_utf8(text_.substr(1));
}

std::vector<ComposerView::Skill> ComposerView::skill_matches() const {
    const auto q = skill_query();
    std::vector<Skill> out;
    if (!q || !skill_list_) return out;
    auto lower = [](std::string s) {
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    const std::string needle = lower(*q);
    for (const Skill& s : *skill_list_)
        if (lower(s.name).find(needle) != std::string::npos) out.push_back(s);
    return out;
}

void ComposerView::rebuild_skills() {
    skills_->clear_children();
    const auto q = skill_query();
    skills_->visible = q.has_value();
    if (!q) {
        skills_->relayout();
        return;
    }
    if (!skill_list_) {
        skill_list_.emplace();
        std::weak_ptr<int> alive = alive_;
        shell_.call("skills.list", json::object(), [this, alive](Result<json> r) {
            if (!alive.lock() || !r) return;
            skill_list_->clear();
            if (const json* a = jobj(*r, "skills"); a && a->is_array())
                for (const json& s : *a) skill_list_->push_back({jstr(s, "name"), jstr(s, "description")});
            rebuild_skills();
            relayout();
        });
    }
    ui::Ui& u = shell_.ui();
    skills_->emplace<ui::Label>(u, shell_.t("composer.skills_label"), TextStyle::caption, Paint::text_secondary, true,
                                "composer-skills-label");
    const auto matches = skill_matches();
    if (matches.empty()) {
        skills_->emplace<ui::Label>(u, shell_.t("composer.no_skills"), TextStyle::body, Paint::text_secondary, true,
                                    "composer-no-skills");
    } else {
        auto* list = skills_->emplace<Node>("composer-skill-list");
        list->role = ui::Role::list;
        list->name = shell_.t("composer.skills_label");
        for (size_t i = 0; i < matches.size(); ++i)
            list->emplace<SkillOption>(u, "skill-" + matches[i].name, ui::to_utf16("/" + matches[i].name),
                                       ui::to_utf16(matches[i].description), i == std::min(skill_index_, matches.size() - 1),
                                       [this, i] { choose_skill(i); });
    }
    skills_->relayout();
    skills_->invalidate();
}

void ComposerView::choose_skill(size_t index) {
    const auto matches = skill_matches();
    if (index >= matches.size()) return;
    field_->set_text(ui::to_utf16("/" + matches[index].name + " "));
    text_changed();  // set_text does not report the change
    if (ui::Tree* tr = Node::tree()) tr->set_focus(field_);
}

// ---- attachments ---------------------------------------------------------------------------------------------------

void ComposerView::pick() {
    const auto paths = shell_.pick_files();
    if (!paths.empty()) attach(paths);
}

void ComposerView::attach(const std::vector<std::wstring>& paths) {
    show_error({});
    std::weak_ptr<int> alive = alive_;
    for (const std::wstring& p : paths) {
        const std::filesystem::path path(p);
        const std::wstring wname = path.filename().wstring();
        const std::u16string name16(wname.begin(), wname.end());
        if (files_.size() + attaching_ >= kMaxFiles) {
            show_error(shell_.t("composer.too_many"));
            return;
        }
        std::error_code ec;
        const auto size = std::filesystem::file_size(path, ec);
        if (ec) {
            show_error(shell_.t("common.error_prefix") + u" " + ui::to_utf16(ec.message()));
            continue;
        }
        if (static_cast<long long>(size) > kMaxBytes) {
            show_error(shell_.t("composer.too_large", {{"name", name16}}));
            continue;
        }
        std::ifstream in(path, std::ios::binary);
        const std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (!in && !in.eof()) {
            show_error(shell_.t("common.error_prefix") + u" " + name16);
            continue;
        }
        ++attaching_;
        const std::string b64 = base64_encode(reinterpret_cast<const std::uint8_t*>(data.data()), data.size());
        shell_.call("drafts.attach", {{"thread_key", key_}, {"name", ui::to_utf8(name16)}, {"data", b64}},
                    [this, alive](Result<json> r) {
                        if (!alive.lock()) return;
                        --attaching_;
                        if (!r) return show_error(ui::to_utf16(r.error().message));
                        files_.push_back({jstr(*r, "id"), jstr(*r, "name"), jint(*r, "size")});
                        rebuild_attachments();
                        relayout();
                    });
    }
}

void ComposerView::rebuild_attachments() {
    attachments_->clear_children();
    attachments_->visible = !files_.empty();
    ui::Ui& u = shell_.ui();
    for (const Attachment& a : files_) {
        const std::u16string name16 = ui::to_utf16(a.name);
        auto* chip = attachments_->emplace<ui::Box>("attachment-" + a.id, Paint::surface, 12, Paint::border);
        chip->role = ui::Role::list_item;
        chip->name = name16 + u", " + ui::to_utf16(format_bytes(static_cast<double>(a.size), shell_.decimal()));
        chip->axis = ui::Axis::row;
        chip->width = Sizing::fit;
        chip->gap = 6;
        chip->padding = {10, 2, 2, 2};
        auto* l = chip->emplace<ui::Label>(u, chip->name, TextStyle::caption, Paint::text, false);
        l->role = ui::Role::none;
        l->padding.top = 5;
        auto* x = chip->emplace<ui::IconButton>(u, "attachment-" + a.id + "-remove", ui::icon::cancel,
                                                shell_.t("composer.remove_attachment", {{"name", name16}}));
        const std::string id = a.id;
        std::weak_ptr<int> alive = alive_;
        x->on_click = [this, alive, id] {
            shell_.call("drafts.detach", {{"thread_key", key_}, {"id", id}}, [this, alive, id](Result<json> r) {
                if (!alive.lock()) return;
                if (!r) return show_error(ui::to_utf16(r.error().message));
                std::erase_if(files_, [&](const Attachment& f) { return f.id == id; });
                rebuild_attachments();
                relayout();
                if (ui::Tree* tr = Node::tree()) tr->set_focus(field_);
            });
        };
    }
    attachments_->relayout();
    attachments_->invalidate();
}

// ---- send, queue, steer, stop --------------------------------------------------------------------------------------

void ComposerView::send(bool steer) {
    if (busy_ || (!shell_.runs_blocked().empty() && !shell_.offline())) return;
    if (blank(text_) && files_.empty()) return;
    busy_ = true;
    rebuild_bar();
    if (save_timer_) shell_.window().cancel_timer(save_timer_);
    save_timer_ = 0;
    dirty_ = false;  // the host takes the draft with the message
    const auto& snap = shell_.feed();
    const bool running = key_ != "new" && snap && snap->entry.id == key_ && running_run(*snap);
    std::weak_ptr<int> alive = alive_;
    shell_.call("inbox.send", {{"thread_key", key_}, {"text", ui::to_utf8(text_)}, {"mode", steer ? "steer_now" : "queue"}},
                [this, alive, steer, running](Result<json> r) {
                    if (!alive.lock()) return;
                    busy_ = false;
                    if (!r) {
                        dirty_ = true;
                        rebuild_bar();
                        return show_error(ui::to_utf16(r.error().message));
                    }
                    text_.clear();
                    field_->set_text(u"");
                    files_.clear();
                    show_error({});
                    rebuild_attachments();
                    rebuild_skills();
                    rebuild_bar();
                    relayout();
                    shell_.announce(shell_.t(r->contains("run_id")           ? "composer.sent"
                                             : steer && running ? "composer.steered"
                                             : shell_.offline()  ? "composer.queued_offline"
                                                                : "composer.queued"));
                    const std::string thread_id = jstr(*r, "thread_id");
                    if (auto sent = on_sent) sent(thread_id);  // may replace this composer (a new Thread opens)
                });
}

void ComposerView::stop() {
    const auto& snap = shell_.feed();
    const RunInfo* active = snap && snap->entry.id == key_ ? active_run(*snap) : nullptr;
    if (!active) return;
    std::weak_ptr<int> alive = alive_;
    shell_.call("runs.cancel", {{"run_id", active->id}}, [this, alive](Result<json> r) {
        if (!alive.lock()) return;
        if (!r) return show_error(ui::to_utf16(r.error().message));
        shell_.announce(shell_.t("composer.stopping"));
    });
}

void ComposerView::show_error(const std::u16string& text) {
    error_->set_text(text);
    const bool was = error_->visible;
    error_->visible = !text.empty();
    if (!text.empty()) shell_.announce(text);  // like the web UI's alert
    if (was != error_->visible) relayout();
}

// ---- state from the snapshot and the status ------------------------------------------------------------------------

void ComposerView::update() {
    const auto& snap = shell_.feed();
    const bool mine = key_ != "new" && snap && snap->entry.id == key_;
    // Queued messages (removable until they are sent).
    const bool offline = shell_.offline();
    std::string qsig = offline ? "offline\n" : "";
    if (mine)
        for (const QueuedMessage& q : snap->queued) qsig += std::to_string(q.id) + "|" + q.mode + "|" + q.text + "\n";
    if (qsig != queued_sig_) {
        queued_sig_ = qsig;
        rebuild_queued();
    }
    // New Runs unavailable; offline, the request waits in the Thread (add-offline-operation D3).
    const std::string blocked = shell_.runs_blocked();
    std::u16string notice;
    if (offline)
        notice = shell_.t("composer.offline");
    else if (!blocked.empty())
        notice = blocked == "disconnected" || connection_kind(shell_.status()) == "disconnected"
                     ? shell_.t("composer.disconnected")
                     : shell_.t("composer.unavailable", {{"why", ui::to_utf16(blocked)}});
    if (notice != blocked_->text() || blocked_->visible != !notice.empty()) {
        blocked_->set_text(notice);
        blocked_->visible = !notice.empty();
        blocked_->role = notice.empty() ? ui::Role::none : ui::Role::text;
        relayout();
    }
    rebuild_bar();
}

void ComposerView::rebuild_queued() {
    queued_->clear_children();
    const auto& snap = shell_.feed();
    const bool mine = key_ != "new" && snap && snap->entry.id == key_ && !snap->queued.empty();
    queued_->visible = mine;
    if (mine) {
        ui::Ui& u = shell_.ui();
        auto* h = queued_->emplace<ui::Label>(u, shell_.t(shell_.offline() ? "composer.queued_offline_title" : "composer.queued_title"),
                                              TextStyle::caption, Paint::text_secondary,
                                              true, "composer-queued-title");
        h->role = ui::Role::heading;
        for (const QueuedMessage& q : snap->queued) {
            const std::string qid = std::to_string(q.id);
            auto* row = queued_->emplace<ui::Box>("queued-" + qid, Paint::none, 4, Paint::border);
            row->axis = ui::Axis::row;
            row->gap = 8;
            row->padding = ui::Insets::xy(8, 2);
            if (q.mode == "steer_now")
                row->emplace<ui::Badge>(u, shell_.t("composer.steering"), Paint::surface_alt, Paint::text_secondary);
            // The text wraps, so Edit and Remove stay inside a narrow reading pane.
            auto* l = row->emplace<ui::Label>(u, ui::to_utf16(q.text), TextStyle::body, Paint::text, true, "queued-" + qid + "-text");
            l->width = Sizing::fill;
            l->padding.top = 6;
            auto* e = row->emplace<ui::Button>(u, "queued-" + qid + "-edit", shell_.t("composer.edit_queued"), ui::Button::Kind::subtle);
            e->width = Sizing::fit;
            e->help = ui::to_utf16(q.text);
            e->on_click = [this, alive = std::weak_ptr<int>(alive_), id = q.id, text = q.text] {
                shell_.ask_text_value("queued-edit-dialog", shell_.t("composer.edit_queued_title"), shell_.t("composer.queued_text"),
                                      ui::to_utf16(text), [this, alive, id](const std::string& t) {
                                          if (!alive.lock()) return;
                                          shell_.call("queue.edit", {{"id", id}, {"text", t}}, [this, alive](Result<json> r) {
                                              if (!alive.lock()) return;
                                              if (!r) return show_error(ui::to_utf16(r.error().message));
                                              shell_.announce(shell_.t("composer.edited"));
                                          });
                                      });
            };
            auto* b = row->emplace<ui::Button>(u, "queued-" + qid + "-remove", shell_.t("composer.remove_queued"),
                                               ui::Button::Kind::subtle);
            b->width = Sizing::fit;
            b->help = ui::to_utf16(q.text);
            const long long id = q.id;
            std::weak_ptr<int> alive = alive_;
            b->on_click = [this, alive, id] {
                shell_.call("queue.remove", {{"id", id}}, [this, alive](Result<json> r) {
                    if (!alive.lock()) return;
                    if (!r) return show_error(ui::to_utf16(r.error().message));
                    shell_.announce(shell_.t("composer.removed"));
                    if (ui::Tree* tr = Node::tree()) tr->set_focus(field_);
                });
            };
        }
    }
    queued_->relayout();
    queued_->invalidate();
}

void ComposerView::rebuild_bar() {
    const auto& snap = shell_.feed();
    const bool mine = key_ != "new" && snap && snap->entry.id == key_;
    const bool running = mine && running_run(*snap);
    const RunInfo* active = mine ? active_run(*snap) : nullptr;
    const bool blocked = !shell_.runs_blocked().empty() && !shell_.offline();
    const std::string state = std::string(running ? "r" : "") + (active ? "a" : "") + (blocked ? "b" : "") + (busy_ ? "s" : "") +
                              shell_.ui().strings.lang();
    if (state == bar_state_) return;
    bar_state_ = state;
    ui::Tree* tr = Node::tree();
    std::string focused;  // a bar button keeps the focus when the bar changes
    for (Node* n = tr ? tr->focused() : nullptr; n; n = n->parent())
        if (n->parent() == bar_) focused = n->automation_id();
    bar_->clear_children();
    ui::Ui& u = shell_.ui();
    auto* attach = bar_->emplace<ui::Button>(u, "composer-attach", shell_.t("composer.attach"), ui::Button::Kind::subtle, ui::icon::attach);
    attach->width = Sizing::fit;
    attach->on_click = [this] { pick(); };
    bar_->emplace<DictateControl>(shell_, "composer-dictate", "thread:" + key_, alive_, [this] { return field_; });
    auto* hint = bar_->emplace<ui::Label>(u, shell_.t(running ? "composer.hint_running" : "composer.hint_idle"), TextStyle::caption,
                                          Paint::text_secondary, true, "composer-hint");
    hint->padding.top = 7;
    if (active) {
        auto* stop = bar_->emplace<ui::Button>(u, "composer-stop", shell_.t("composer.stop"), ui::Button::Kind::standard, ui::icon::stop);
        stop->width = Sizing::fit;
        stop->on_click = [this] { this->stop(); };
    }
    if (running) {
        auto* steer = bar_->emplace<ui::Button>(u, "composer-steer", shell_.t("composer.steer"));
        steer->width = Sizing::fit;
        steer->enabled = !busy_ && !blocked;
        steer->on_click = [this] { send(true); };
    }
    auto* send_b = bar_->emplace<ui::Button>(u, "composer-send", shell_.t(running ? "composer.queue" : "composer.send"),
                                             ui::Button::Kind::primary, running ? 0 : ui::icon::send);
    send_b->width = Sizing::fit;
    send_b->enabled = !busy_ && !blocked;
    send_b->on_click = [this] { send(false); };
    if (tr && !focused.empty())
        for (const auto& c : bar_->children())
            if (c->automation_id() == focused && c->enabled) tr->set_focus(c.get());
    bar_->relayout();
    bar_->invalidate();
}

}  // namespace wb::shell
