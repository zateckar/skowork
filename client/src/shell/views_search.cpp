// Full-Text Search (client/work-inbox): titles and messages of all Threads, archived ones included. The view takes
// the Thread list's place; opening a hit opens its Thread scrolled to the matching message (FeedView::jump).
#include "shell/format.h"
#include "shell/shell.h"
#include "ui/richtext.h"
#include "ui/text16.h"
#include "ui/textfield.h"

namespace wb::shell {

using ui::Node;
using ui::Paint;
using ui::Sizing;
using ui::TextStyle;

namespace {

constexpr UINT kSearchDelayMs = 200;

// One hit: the Thread's title (and Archived), then the snippet with the matches in bold. A link to the message.
class HitLink : public ui::Box {
public:
    HitLink(Shell& shell, std::string id, const json& h) : Box(std::move(id)), shell_(shell) {
        role = ui::Role::link;
        focusable = true;
        padding = ui::Insets::xy(14, 8);
        gap = 2;
        const std::string thread = jstr(h, "thread_id");
        const std::string run = jstr(h, "run_id");
        hash_ = run.empty() ? thread_hash(thread) : thread_hash(thread, MessageRef{run, jint(h, "seq")});
        const std::string title = jstr(h, "title");
        const std::u16string t16 = title.empty() ? shell.t("list.untitled") : ui::to_utf16(title);
        auto* head = emplace<Node>();
        head->axis = ui::Axis::row;
        head->gap = 6;
        head->emplace<ui::Label>(shell.ui(), t16, TextStyle::body_strong, Paint::text, false)->role = ui::Role::none;
        name = t16;
        if (jbool(h, "archived")) {
            head->emplace<ui::Badge>(shell.ui(), shell.t("search.archived"), Paint::surface_alt, Paint::text)->role = ui::Role::none;
            name += u", " + shell.t("search.archived");
        }
        std::u16string text;
        std::vector<ui::Span> spans;
        for (const SnippetPart& p : snippet_parts(jstr(h, "snippet"))) {
            const std::u16string s = ui::to_utf16(p.text);
            if (p.hit) spans.push_back({static_cast<uint32_t>(text.size()), static_cast<uint32_t>(s.size()), ui::kBold, {}});
            text += s;
        }
        if (!text.empty()) {
            emplace<ui::RichText>(shell.ui(), text, std::move(spans), TextStyle::caption, Paint::text_secondary)->role = ui::Role::none;
            name += u", " + text;
        }
    }
    bool can_invoke() const override { return true; }
    void invoke() override {
        // Closing the search hides this hit; navigate with copies.
        Shell& s = shell_;
        const std::string hash = hash_;
        s.close_search();
        s.navigate(hash);
    }
    int cursor() const override { return 1; }
    void draw(ui::Canvas& c) override {
        if (pressed_) c.round(frame(), 4, Paint::text, Paint::none, 0.08f);
        else if (hovered_) c.round(frame(), 4, Paint::text, Paint::none, 0.04f);
        const ui::Rect f = frame();
        c.round({f.x, f.bottom() - 1, f.w, 1}, 0, Paint::border);
    }

private:
    Shell& shell_;
    std::string hash_;
};

std::string trimmed(const std::u16string& s) {
    std::string t = ui::to_utf8(s);
    const auto b = t.find_first_not_of(" \t\r\n");
    return b == std::string::npos ? std::string() : t.substr(b, t.find_last_not_of(" \t\r\n") - b + 1);
}

}  // namespace

SearchView::SearchView(Shell& shell) : Node("search-panel"), shell_(shell) {
    role = ui::Role::group;
    name = shell.t("search.label");
    height = Sizing::fill;
    gap = 4;
    auto* row = emplace<Node>("search-row");
    row->axis = ui::Axis::row;
    row->gap = 6;
    row->padding = ui::Insets::xy(10, 6);
    ui::TextField::Options o;
    o.placeholder = shell.t("search.placeholder");
    field_ = row->emplace<ui::TextField>(shell.ui(), "search-field", shell.t("search.label"), o);
    field_->width = Sizing::fill;
    field_->on_change = [this] { changed(); };
    field_->on_key_down = [this](const ui::KeyEvent& e) {
        if (e.ctrl || e.alt) return false;
        if (e.key == ui::kKeyEscape) {
            shell_.close_search();
            return true;
        }
        if (e.key == ui::kKeyDown && !results_->children().empty()) {
            shell_.window().tree().set_focus(results_->children().front().get());
            return true;
        }
        return false;
    };
    auto* close = row->emplace<ui::Button>(shell.ui(), "search-close", shell.t("search.close"), ui::Button::Kind::subtle);
    close->on_click = [this] { shell_.close_search(); };
    status_ = emplace<ui::Label>(shell.ui(), u"", TextStyle::caption, Paint::text_secondary, true, "search-status");
    status_->padding = ui::Insets::xy(14, 2);
    status_->visible = false;
    auto* scroll = emplace<ui::ScrollView>("search-scroll");
    scroll->scrolls = true;
    scroll->height = Sizing::fill;
    results_ = scroll->emplace<Node>("search-results");
    results_->role = ui::Role::list;
    results_->name = shell.t("search.label");
}

SearchView::~SearchView() = default;

void SearchView::reset() {
    ++generation_;
    if (timer_) shell_.window().cancel_timer(timer_);
    timer_ = 0;
    field_->set_text(u"");
    status_->visible = false;
    results_->clear_children();
    relayout();
}

// Searches as the user types, 200 ms after the last edit; replies to older queries are dropped.
void SearchView::changed() {
    const std::string q = trimmed(field_->text());
    const uint64_t gen = ++generation_;
    if (timer_) shell_.window().cancel_timer(timer_);
    timer_ = 0;
    if (q.empty()) {
        status_->visible = false;
        results_->clear_children();
        relayout();
        invalidate();
        return;
    }
    std::weak_ptr<int> alive = alive_;
    timer_ = shell_.window().set_timer(kSearchDelayMs, [this, alive, q, gen] {
        if (!alive.lock() || gen != generation_) return;
        timer_ = 0;
        query(q);
    });
}

void SearchView::query(const std::string& q) {
    const uint64_t gen = generation_;
    std::weak_ptr<int> alive = alive_;
    shell_.call("search.query", {{"q", q}}, [this, alive, gen](Result<json> r) {
        if (!alive.lock() || gen != generation_) return;
        status_->visible = true;
        if (!r) {
            status_->color = Paint::error;
            status_->set_text(shell_.t("common.error_prefix") + u" " + ui::to_utf16(r.error().message));
            shell_.announce(status_->text());
            relayout();
            return;
        }
        const json* hits = r->contains("hits") ? &(*r)["hits"] : nullptr;
        show(hits && hits->is_array() ? *hits : json::array());
    });
}

void SearchView::show(const json& hits) {
    results_->clear_children();
    size_t i = 0;
    for (const json& h : hits) results_->emplace<HitLink>(shell_, "search-hit-" + std::to_string(i++), h);
    const auto n = static_cast<long long>(hits.size());
    status_->color = Paint::text_secondary;
    status_->set_text(n == 0 ? shell_.t("search.none") : shell_.count("search.results", n));
    shell_.announce(n == 0 ? shell_.t("search.none") : shell_.count("search.results", n));
    relayout();
    invalidate();
}

}  // namespace wb::shell
