#include "ui/layout.h"

#include <algorithm>
#include <atomic>
#include <utility>

namespace wb::ui {

Rect Rect::intersect(const Rect& o) const {
    const float l = std::max(x, o.x), t = std::max(y, o.y);
    const float r = std::min(right(), o.right()), b = std::min(bottom(), o.bottom());
    if (r <= l || b <= t) return {};
    return {l, t, r - l, b - t};
}

Rect Rect::unite(const Rect& o) const {
    if (o.empty()) return *this;
    if (empty()) return o;
    const float l = std::min(x, o.x), t = std::min(y, o.y);
    return {l, t, std::max(right(), o.right()) - l, std::max(bottom(), o.bottom()) - t};
}

const char* role_name(Role r) {
    switch (r) {
    case Role::none: return "none";
    case Role::pane: return "pane";
    case Role::group: return "group";
    case Role::text: return "text";
    case Role::document: return "document";
    case Role::button: return "button";
    case Role::checkbox: return "checkbox";
    case Role::link: return "link";
    case Role::list: return "list";
    case Role::list_item: return "list_item";
    case Role::menu: return "menu";
    case Role::menu_item: return "menu_item";
    case Role::scroll_view: return "scroll_view";
    case Role::splitter: return "splitter";
    case Role::tooltip: return "tooltip";
    case Role::image: return "image";
    case Role::status_bar: return "status_bar";
    case Role::separator: return "separator";
    case Role::edit: return "edit";
    case Role::dialog: return "dialog";
    case Role::heading: return "heading";
    }
    return "?";
}

// --- Node ---------------------------------------------------------------------------------------------------

namespace {
std::atomic<uint32_t> g_next_runtime_id{1};
}

Node::Node(std::string automation_id) : id_(std::move(automation_id)), runtime_id_(g_next_runtime_id++) {}

Node* Node::add(std::unique_ptr<Node> child) {
    child->parent_ = this;
    children_.push_back(std::move(child));
    relayout();
    return children_.back().get();
}

Node* Node::insert(size_t pos, std::unique_ptr<Node> child) {
    child->parent_ = this;
    pos = std::min(pos, children_.size());
    children_.insert(children_.begin() + static_cast<std::ptrdiff_t>(pos), std::move(child));
    relayout();
    return children_[pos].get();
}

std::unique_ptr<Node> Node::remove(Node* child) {
    for (auto it = children_.begin(); it != children_.end(); ++it) {
        if (it->get() == child) {
            if (Tree* t = tree()) {
                t->forget(child);
                t->invalidate(*child);
                t->request_layout();
            }
            std::unique_ptr<Node> out = std::move(*it);
            children_.erase(it);
            out->parent_ = nullptr;
            return out;
        }
    }
    return nullptr;
}

void Node::clear_children() {
    while (!children_.empty()) remove(children_.back().get());
}

Tree* Node::tree() const {
    const Node* n = this;
    while (n->parent_) n = n->parent_;
    return n->tree_;
}

void Node::invalidate() const {
    if (Tree* t = tree()) t->invalidate(*this);
}

void Node::relayout() const {
    if (Tree* t = tree()) t->request_layout();
}

float Node::max_scroll() const {
    const float inner = frame_.h - padding.top - padding.bottom;
    return std::max(0.f, content_h_ - inner);
}

// --- Tree: layout ---------------------------------------------------------------------------------------------

Tree::Tree() : root_(std::make_unique<Node>("root")) {
    root_->tree_ = this;
    root_->width = Sizing::fill;
    root_->height = Sizing::fill;
    root_->role = Role::pane;
}

Tree::~Tree() {
    root_->tree_ = nullptr;  // children being destroyed must not call back into a dying tree
}

namespace {
// The lines of a wrapping row: children [first, last) and the width their fit and fixed sizes and gaps use. widths
// receives each child's width (a fill child's min_w until its line's free space is shared out).
struct WrapLine {
    size_t first = 0, last = 0;
    float used = 0;
};
std::vector<WrapLine> wrap_lines(Tree& t, const std::vector<Node*>& kids, float inner, float gap, std::vector<float>& widths) {
    widths.assign(kids.size(), 0.f);
    std::vector<WrapLine> lines;
    WrapLine cur;
    for (size_t i = 0; i < kids.size(); ++i) {
        Node* k = kids[i];
        const float w = k->width == Sizing::fill ? k->min_w : std::min(t.measure(*k, inner).w, inner);
        widths[i] = w;
        const bool empty = cur.last == cur.first;
        if (!empty && cur.used + gap + w > inner + 0.5f) {
            lines.push_back(cur);
            cur = {i, i + 1, w};
            continue;
        }
        cur.used += (empty ? 0.f : gap) + w;
        if (empty) cur.first = i;
        cur.last = i + 1;
    }
    if (cur.last > cur.first) lines.push_back(cur);
    return lines;
}
}  // namespace

Size Tree::measure(Node& n, float avail_w) {
    if (n.measured_pass_ == pass_ && n.measured_for_ == avail_w) return n.desired_;
    const float pad_h = n.padding.left + n.padding.right, pad_v = n.padding.top + n.padding.bottom;
    float w = n.width == Sizing::fixed ? n.fixed_w : avail_w;
    const float inner = std::max(0.f, w - pad_h);

    Size c;
    std::vector<Node*> kids;
    for (auto& ch : n.children_)
        if (ch->visible && !ch->floating) kids.push_back(ch.get());
    const float gaps = kids.size() > 1 ? n.gap * static_cast<float>(kids.size() - 1) : 0.f;
    if (n.measure) {
        c = n.measure(inner);
    } else if (kids.empty()) {
    } else if (n.axis == Axis::column) {
        for (Node* k : kids) {
            const Size s = measure(*k, inner);
            c.w = std::max(c.w, s.w);
            c.h += s.h;
        }
        c.h += gaps;
    } else if (n.wraps) {
        std::vector<float> widths;
        const auto lines = wrap_lines(*this, kids, inner, n.gap, widths);
        for (size_t l = 0; l < lines.size(); ++l) {
            float lh = 0;
            for (size_t i = lines[l].first; i < lines[l].last; ++i) lh = std::max(lh, measure(*kids[i], widths[i]).h);
            c.h += lh + (l > 0 ? n.gap : 0.f);
            c.w = std::max(c.w, lines[l].used);
        }
    } else {
        float used = gaps, weights = 0;
        for (Node* k : kids) {
            if (k->width == Sizing::fill) {
                weights += k->weight;
                continue;
            }
            const Size s = measure(*k, inner);
            used += s.w;
            c.h = std::max(c.h, s.h);
        }
        const float free = std::max(0.f, inner - used);
        float fill_w = 0;
        for (Node* k : kids) {
            if (k->width != Sizing::fill) continue;
            const float share = weights > 0 ? free * k->weight / weights : 0.f;
            const Size s = measure(*k, share);
            fill_w += s.w;
            c.h = std::max(c.h, s.h);
        }
        c.w = used + fill_w;
    }
    if (n.width == Sizing::fit) w = c.w + pad_h;
    const float h = n.height == Sizing::fixed ? n.fixed_h : c.h + pad_v;
    n.desired_ = {std::max(w, n.min_w), std::max(h, n.min_h)};
    n.measured_pass_ = pass_;
    n.measured_for_ = avail_w;
    return n.desired_;
}

void Tree::place(Node& n, const Rect& r) { arrange(n, r); }

void Tree::arrange(Node& n, const Rect& r) {
    if (!(n.frame_ == r)) {
        if (!n.frame_.empty()) invalidate(visible_rect(n));
        const bool resized = n.frame_.w != r.w || n.frame_.h != r.h;
        n.frame_ = r;
        invalidate(visible_rect(n));
        if (resized) n.on_frame_changed();
    }
    const Rect in{r.x + n.padding.left, r.y + n.padding.top, std::max(0.f, r.w - n.padding.left - n.padding.right),
                  std::max(0.f, r.h - n.padding.top - n.padding.bottom)};
    if (n.arrange_children(*this, in)) return;
    std::vector<Node*> kids;
    for (auto& ch : n.children_) {
        if (ch->visible && ch->floating) continue;
        if (ch->visible) kids.push_back(ch.get());
        else if (!ch->frame_.empty()) {
            invalidate(visible_rect(*ch));
            ch->frame_ = {};
        }
    }
    struct Floating {
        Tree& t;
        Node& n;
        const Rect& in;
        ~Floating() {
            for (auto& ch : n.children_)
                if (ch->visible && ch->floating) t.arrange(*ch, in);
        }
    } floating{*this, n, in};
    if (kids.empty()) {
        n.content_h_ = 0;
        return;
    }
    const float gaps = kids.size() > 1 ? n.gap * static_cast<float>(kids.size() - 1) : 0.f;
    const bool col = n.axis == Axis::column || n.scrolls;
    if (!col && n.wraps) {
        std::vector<float> widths;
        float y = in.y;
        for (const WrapLine& l : wrap_lines(*this, kids, in.w, n.gap, widths)) {
            float weights = 0;
            for (size_t i = l.first; i < l.last; ++i)
                if (kids[i]->width == Sizing::fill) weights += kids[i]->weight;
            const float free = std::max(0.f, in.w - l.used);
            for (size_t i = l.first; i < l.last; ++i)
                if (kids[i]->width == Sizing::fill && weights > 0) widths[i] += free * kids[i]->weight / weights;
            float lh = 0;
            for (size_t i = l.first; i < l.last; ++i) lh = std::max(lh, measure(*kids[i], widths[i]).h);
            float x = in.x + (weights == 0 && n.align_end ? free : 0.f);
            for (size_t i = l.first; i < l.last; ++i) {
                const float h = kids[i]->height == Sizing::fill ? lh : measure(*kids[i], widths[i]).h;
                arrange(*kids[i], {x, y, widths[i], h});
                x += widths[i] + n.gap;
            }
            y += lh + n.gap;
        }
        return;
    }

    // Main-axis sizes: fixed and fit first, then fill shares of what is left. A scrolling column gives every
    // child its content height.
    std::vector<float> main(kids.size()), cross(kids.size());
    float used = gaps, weights = 0;
    for (size_t i = 0; i < kids.size(); ++i) {
        Node* k = kids[i];
        const Sizing m = col ? k->height : k->width;
        if (m == Sizing::fill && !n.scrolls) {
            weights += k->weight;
            continue;
        }
        const Size s = measure(*k, in.w);
        main[i] = col ? s.h : std::min(s.w, k->width == Sizing::fixed ? s.w : in.w);
        used += main[i];
    }
    const float free = std::max(0.f, (col ? in.h : in.w) - used);
    for (size_t i = 0; i < kids.size(); ++i) {
        Node* k = kids[i];
        if ((col ? k->height : k->width) != Sizing::fill || n.scrolls) continue;
        main[i] = std::max(col ? k->min_h : k->min_w, weights > 0 ? free * k->weight / weights : 0.f);
    }
    // Cross-axis sizes.
    for (size_t i = 0; i < kids.size(); ++i) {
        Node* k = kids[i];
        if (col) {
            const Size s = measure(*k, in.w);
            cross[i] = k->width == Sizing::fixed ? s.w : std::min(s.w, in.w);
        } else if (k->height == Sizing::fill) {
            cross[i] = in.h;
        } else {
            cross[i] = measure(*k, main[i]).h;
        }
    }
    if (n.scrolls) {
        const bool at_end = n.follow_end && n.scroll_y_ >= n.content_h_ - in.h - 2.f;
        n.content_h_ = used;
        n.scroll_y_ = at_end ? std::max(0.f, used - in.h) : std::clamp(n.scroll_y_, 0.f, std::max(0.f, used - in.h));
    }
    float pos = col ? in.y - n.scroll_y_ : in.x;
    for (size_t i = 0; i < kids.size(); ++i) {
        const Rect cr = col ? Rect{in.x, pos, cross[i], main[i]} : Rect{pos, in.y, main[i], cross[i]};
        arrange(*kids[i], cr);
        pos += main[i] + n.gap;
    }
}

void Tree::layout(Size viewport) {
    if (viewport.w != viewport_.w || viewport.h != viewport_.h) {
        viewport_ = viewport;
        invalidate_all();
    }
    if (on_layout) on_layout(viewport);
    Node& r = *root_;
    // A node sized by where it was placed (CaptionSpace) asks for one more pass; such sizes settle after one.
    for (int i = 0; i < 3; ++i) {
        again_ = false;
        ++pass_;
        const Size s = measure(r, viewport.w);
        arrange(r, {0, 0, r.width == Sizing::fill ? viewport.w : s.w, r.height == Sizing::fill ? viewport.h : s.h});
        if (!again_) break;
    }
    layout_requested_ = false;  // rows a virtual list realised during this pass are already placed
}

// --- Tree: hit testing, invalidation, scrolling ---------------------------------------------------------------

namespace {
Node* hit(Node& n, float x, float y) {
    if (!n.visible || !n.frame().contains(x, y)) return nullptr;
    const auto& kids = n.children();
    for (auto it = kids.rbegin(); it != kids.rend(); ++it)
        if (Node* h = hit(**it, x, y)) return h;
    return n.hit_through ? nullptr : &n;
}

void collect(Node& n, std::vector<Node*>& out) {
    if (!n.visible || !n.enabled) return;
    if (n.focusable) out.push_back(&n);
    for (auto& c : n.children()) collect(*c, out);
}

bool within(const Node* n, const Node* subtree) {
    for (; n; n = n->parent())
        if (n == subtree) return true;
    return false;
}

bool interactive(const Node& n) {
    return n.focusable || n.can_invoke() || n.toggle_state() >= 0 || n.selected_state() >= 0 || n.expand_state() >= 0;
}

Node* interactive_target(Node* n) {
    for (; n; n = n->parent())
        if (n->enabled && interactive(*n)) return n;
    return nullptr;
}

Node* focusable_ancestor(Node* n) {
    for (; n; n = n->parent())
        if (n->focusable && n->enabled && n->visible) return n;
    return nullptr;
}

// The default action of a node: invoke, toggle, select or expand/collapse.
bool activate(Node& n) {
    if (!n.enabled) return false;
    if (n.can_invoke()) n.invoke();
    else if (n.toggle_state() >= 0) n.toggle();
    else if (n.selected_state() >= 0) n.select();
    else if (n.expand_state() >= 0) n.set_expanded(n.expand_state() == 0);
    else return false;
    return true;
}
}  // namespace

Node* Tree::hit_test(float x, float y) const { return hit(*root_, x, y); }

Rect Tree::visible_rect(const Node& n) const {
    Rect r = n.frame();
    for (const Node* p = n.parent(); p; p = p->parent())
        if (p->scrolls || p->clips) r = r.intersect(p->frame());
    return r.intersect({0, 0, viewport_.w, viewport_.h});
}

void Tree::invalidate(const Rect& r0) {
    Rect r = r0.intersect({0, 0, viewport_.w, viewport_.h});
    if (r.empty()) return;
    // Merge with every rectangle it overlaps, or that it would cover cheaply, until none does.
    for (bool merged = true; merged;) {
        merged = false;
        for (auto it = dirty_.begin(); it != dirty_.end(); ++it) {
            const Rect u = it->unite(r);
            if (!it->intersect(r).empty() || u.w * u.h <= it->w * it->h + r.w * r.h) {
                r = u;
                dirty_.erase(it);
                merged = true;
                break;
            }
        }
    }
    dirty_.push_back(r);
    if (dirty_.size() > kMaxDirty) {
        Rect all;
        for (const Rect& d : dirty_) all = all.unite(d);
        dirty_.assign(1, all);
    }
    if (on_dirty) on_dirty();
}

void Tree::invalidate_all() {
    dirty_.clear();
    if (viewport_.w > 0 && viewport_.h > 0) dirty_.push_back({0, 0, viewport_.w, viewport_.h});
    if (on_dirty) on_dirty();
}

std::vector<Rect> Tree::take_dirty() { return std::exchange(dirty_, {}); }

bool Tree::scroll_to(Node& s, float y) {
    if (!s.scrolls) return false;
    const float target = std::clamp(y, 0.f, s.max_scroll());
    if (target == s.scroll_y_) return false;
    s.scroll_y_ = target;
    invalidate(visible_rect(s).inset(-4.f));  // with a focus ring around a row at the edge
    const bool requested = layout_requested_;
    arrange(s, s.frame_);  // moves the children (a virtual list realises rows); the measure cache stays valid
    layout_requested_ = requested;
    return true;
}

bool Tree::scroll_into_view(const Node& n) {
    bool moved = false;
    for (Node* a = n.parent(); a; a = a->parent()) {
        if (!a->scrolls) continue;
        const float top = a->frame().y + a->padding.top;
        const float bottom = a->frame().bottom() - a->padding.bottom;
        const Rect f = n.frame();
        if (f.y < top) moved |= scroll_to(*a, a->scroll_y_ - (top - f.y));
        else if (f.bottom() > bottom) moved |= scroll_to(*a, a->scroll_y_ + std::min(f.bottom() - bottom, f.y - top));
    }
    return moved;
}

// --- Tree: focus and input ------------------------------------------------------------------------------------

std::vector<Node*> Tree::focus_chain() const {
    std::vector<Node*> out;
    collect(modal_ ? *modal_ : *root_, out);
    return out;
}

void Tree::set_modal(Node* n) {
    modal_ = n;
    if (n && !within(focused_, n)) set_focus(next_focus(nullptr));
}

Node* Tree::next_focus(const Node* current, bool backward) const {
    const std::vector<Node*> chain = focus_chain();
    if (chain.empty()) return nullptr;
    const auto it = std::find(chain.begin(), chain.end(), current);
    if (it == chain.end()) return backward ? chain.back() : chain.front();
    const auto i = static_cast<size_t>(it - chain.begin());
    return backward ? chain[(i + chain.size() - 1) % chain.size()] : chain[(i + 1) % chain.size()];
}

void Tree::set_focus(Node* n) {
    if (n == focused_) return;
    Node* old = focused_;
    focused_ = n;
    // The focus ring reaches 4 DIPs outside the focus rectangle.
    if (old) {
        old->on_focus(false);
        invalidate(old->focus_rect().inset(-4.f));
    }
    if (n) {
        n->on_focus(true);
        scroll_into_view(*n);
        invalidate(n->focus_rect().inset(-4.f));
    }
    if (on_focus_changed) on_focus_changed(n);
}

void Tree::forget(const Node* subtree) {
    if (within(modal_, subtree)) modal_ = nullptr;
    if (within(focused_, subtree)) focused_ = nullptr;
    if (within(hovered_, subtree)) hovered_ = nullptr;
    if (within(pressed_, subtree)) pressed_ = nullptr;
    if (within(captured_, subtree)) captured_ = nullptr;
    if (within(delivering_, subtree)) delivering_ = nullptr;
}

bool Tree::deliver(Node& n, const PointerEvent& e, bool& gone) {
    delivering_ = &n;
    const bool handled = n.on_pointer(e);
    gone = delivering_ == nullptr;
    delivering_ = nullptr;
    return handled;
}

bool Tree::pointer(const PointerEvent& e) {
    Node* h = e.kind == PointerEvent::leave ? nullptr : hit_test(e.x, e.y);
    if (modal_ && h && !within(h, modal_)) h = nullptr;  // the content under a modal dialog does not react
    if (captured_ && (e.kind == PointerEvent::move || e.kind == PointerEvent::up)) {
        // A node that took the press through on_pointer (a splitter drag, a text selection) keeps the pointer
        // until release.
        Node* c = captured_;
        bool gone = false;
        const bool handled = deliver(*c, e, gone);
        if (e.kind == PointerEvent::up) {
            captured_ = nullptr;
            if (!gone) {
                c->pressed_ = false;
                invalidate(*c);
            }
        }
        return handled;
    }
    // Bubbles the event from a node up its ancestors until one handles it. A handler that removed its own node changed
    // the tree under the pointer: the event counts as handled and no node of the old tree is touched (handler null).
    const auto bubble = [&](Node* from, Node** handler) {
        for (Node* n = from; n;) {
            bool gone = false;
            const bool handled = deliver(*n, e, gone);
            if (handled || gone) {
                if (handler) *handler = gone ? nullptr : n;
                return true;
            }
            n = n->parent();
        }
        return false;
    };
    switch (e.kind) {
    case PointerEvent::move:
    case PointerEvent::leave: {
        Node* t = interactive_target(h);
        if (t != hovered_) {
            if (hovered_) {
                hovered_->hovered_ = false;
                hovered_->on_hover(false);
                invalidate(*hovered_);
            }
            hovered_ = t;
            if (t) {
                t->hovered_ = true;
                t->on_hover(true);
                invalidate(*t);
            }
        }
        if (bubble(h, nullptr)) return true;
        return t != nullptr;
    }
    case PointerEvent::wheel:
        for (Node* n = h; n; n = n->parent()) {
            bool gone = false;
            if (deliver(*n, e, gone) || gone) return true;
            if (n->scrolls && n->max_scroll() > 0) return scroll_to(*n, n->scroll_y_ + e.wheel_dy) || true;
        }
        return false;
    case PointerEvent::down: {
        Node* n = nullptr;
        if (bubble(h, &n)) {
            if (n) {
                captured_ = n;
                n->pressed_ = true;
                if (Node* f = focusable_ancestor(n)) set_focus(f);
            }
            return true;
        }
        Node* t = interactive_target(h);
        if (Node* f = focusable_ancestor(h)) set_focus(f);
        if (pressed_) pressed_->pressed_ = false;
        pressed_ = t;
        if (t) {
            t->pressed_ = true;
            invalidate(*t);
        }
        return t != nullptr;
    }
    case PointerEvent::up: {
        if (bubble(h, nullptr)) {
            if (pressed_) pressed_->pressed_ = false;  // forget() cleared it if the handler removed it
            pressed_ = nullptr;
            return true;
        }
        Node* t = interactive_target(h);
        Node* p = pressed_;
        pressed_ = nullptr;
        if (!p) return false;
        p->pressed_ = false;
        invalidate(*p);
        if (t == p && e.button == 0) return activate(*p);
        return false;
    }
    }
    return false;
}

bool Tree::key(const KeyEvent& e) {
    if (e.key == kKeyTab && !e.ctrl && !e.alt) {
        set_focus(next_focus(focused_, e.shift));
        return focused_ != nullptr;
    }
    for (Node* n = focused_; n; n = n->parent()) {
        if (n->on_key(e)) return true;
        if (n == modal_) break;  // keys do not leave a modal dialog
    }
    if (modal_ && !focused_) return modal_->on_key(e);
    if (!focused_) return false;
    // Enter and Space press the focused control, also a dialog's buttons.
    if ((e.key == kKeyEnter || e.key == kKeySpace) && !e.ctrl && !e.alt) return activate(*focused_);
    // Page and Home/End keys scroll the nearest scrolling ancestor, within a modal dialog when one is open.
    for (Node* n = focused_; n; n = n == modal_ ? nullptr : n->parent()) {
        if (!n->scrolls) continue;
        const float page = std::max(20.f, n->frame().h - 40.f);
        switch (e.key) {
        case kKeyPageDown: return scroll_to(*n, n->scroll_y_ + page);
        case kKeyPageUp: return scroll_to(*n, n->scroll_y_ - page);
        case kKeyHome: return e.ctrl && scroll_to(*n, 0);
        case kKeyEnd: return e.ctrl && scroll_to(*n, n->max_scroll());
        default: return false;
        }
    }
    return false;
}

// --- accessibility tree -----------------------------------------------------------------------------------------

namespace {
bool exposed(const Node& n) { return n.visible && !n.native_window && (n.role != Role::none || !n.parent()); }

void exposed_children(const Node& n, std::vector<Node*>& out) {
    for (const auto& c : n.children()) {
        if (!c->visible) continue;
        if (exposed(*c)) out.push_back(c.get());
        else exposed_children(*c, out);
    }
}

bool needs_name(Role r) {
    switch (r) {
    case Role::button:
    case Role::checkbox:
    case Role::link:
    case Role::list:
    case Role::list_item:
    case Role::menu:
    case Role::menu_item:
    case Role::splitter:
    case Role::image:
    case Role::document:
    case Role::edit:
    case Role::dialog:
    case Role::heading:
    case Role::text: return true;
    default: return false;
    }
}

std::string label(const Node& n) {
    return std::string(role_name(n.role)) + " '" + (n.automation_id().empty() ? "?" : n.automation_id()) + "'";
}

void check(const Node& n, std::vector<std::string>& out) {
    if (!n.visible) return;
    if (n.focusable && n.role == Role::none && n.parent())
        out.push_back("focusable node '" + n.automation_id() + "' has no role");
    // A field in its own window gets its name from that window's annotation, which is set from `name` too.
    if ((exposed(n) || n.native_window) && n.parent() && needs_name(n.role) && n.name.empty()) out.push_back(label(n) + " has no name");
    const bool acts = n.can_invoke() || n.toggle_state() >= 0 || n.expand_state() >= 0 || n.selected_state() >= 0;
    if (acts && n.enabled && !n.system_keyboard) {
        bool reachable = false;
        for (const Node* a = &n; a && !reachable; a = a->parent()) reachable = a->focusable;
        if (!reachable) out.push_back(label(n) + " cannot be reached by keyboard");
    }
    for (const auto& c : n.children()) check(*c, out);
}
}  // namespace

Node* a11y_parent(const Node& n) {
    Node* p = n.parent();
    while (p && !exposed(*p)) p = p->parent();
    return p;
}

std::vector<Node*> a11y_children(const Node& n) {
    std::vector<Node*> out;
    exposed_children(n, out);
    return out;
}

std::vector<std::string> a11y_problems(const Tree& tree) {
    std::vector<std::string> out;
    check(tree.root(), out);
    return out;
}

// --- VirtualList ------------------------------------------------------------------------------------------

void VirtualList::reset(size_t count, float estimate) {
    heights_.assign(count, estimate);
    tree_.assign(count + 1, 0.0);
    for (size_t i = 1; i <= count; ++i) {  // O(n) Fenwick build
        tree_[i] += estimate;
        const size_t j = i + (i & (~i + 1));
        if (j <= count) tree_[j] += tree_[i];
    }
}

void VirtualList::set_height(size_t i, float h) {
    const double d = static_cast<double>(h) - heights_[i];
    heights_[i] = h;
    for (size_t k = i + 1; k < tree_.size(); k += k & (~k + 1)) tree_[k] += d;
}

float VirtualList::offset(size_t i) const {
    double s = 0;
    for (size_t k = std::min(i, heights_.size()); k > 0; k -= k & (~k + 1)) s += tree_[k];
    return static_cast<float>(s);
}

size_t VirtualList::index_at(float y) const {
    if (heights_.empty()) return 0;
    // Binary lifting: the largest count whose prefix sum is <= y is the index of the item containing y.
    size_t pos = 0;
    double rem = y;
    size_t step = 1;
    while (step * 2 < tree_.size()) step *= 2;
    for (; step > 0; step /= 2) {
        if (pos + step < tree_.size() && tree_[pos + step] <= rem) {
            pos += step;
            rem -= tree_[pos];
        }
    }
    return std::min(pos, heights_.size() - 1);
}

VirtualList::Range VirtualList::visible(float scroll, float viewport, size_t overscan) const {
    if (heights_.empty() || viewport <= 0) return {};
    const size_t first = index_at(std::max(0.f, scroll));
    size_t last = index_at(scroll + viewport);
    if (offset(last) < scroll + viewport) ++last;  // half-open: include the item containing the bottom edge
    return {first > overscan ? first - overscan : 0, std::min(heights_.size(), last + overscan)};
}

}  // namespace wb::ui
