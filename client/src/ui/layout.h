// The element tree of the native window (add-native-ui design D2): row and column stacks with fixed, fit and fill
// sizes, padding and gaps; scrolling containers; dirty-rectangle invalidation; hit testing; the keyboard focus
// chain; input dispatch; and the accessibility data each node exposes to UI Automation (D8). Portable: no Win32, so
// layout, focus order, input and the accessibility tree are unit-tested on every platform. Units are
// device-independent pixels (DIPs).
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace wb::ui {

struct Size {
    float w = 0, h = 0;
};

struct Rect {
    float x = 0, y = 0, w = 0, h = 0;
    float right() const { return x + w; }
    float bottom() const { return y + h; }
    bool empty() const { return w <= 0 || h <= 0; }
    bool contains(float px, float py) const { return px >= x && py >= y && px < right() && py < bottom(); }
    Rect intersect(const Rect& o) const;
    Rect unite(const Rect& o) const;  // the bounding box; an empty side is ignored
    Rect inset(float d) const { return {x + d, y + d, w - 2 * d, h - 2 * d}; }
    bool operator==(const Rect&) const = default;
};

struct Insets {
    float left = 0, top = 0, right = 0, bottom = 0;
    static Insets all(float v) { return {v, v, v, v}; }
    static Insets xy(float x, float y) { return {x, y, x, y}; }
};

enum class Axis { row, column };
enum class Sizing { fixed, fit, fill };  // fit: the content's size; fill: a share of the free space by weight

// The UI Automation role of a node (D8). Nodes with Role::none are layout only and are skipped in the
// accessibility tree; their exposed descendants appear under the nearest exposed ancestor.
enum class Role {
    none,
    pane,
    group,
    text,
    document,
    button,
    checkbox,
    link,
    list,
    list_item,
    menu,
    menu_item,
    scroll_view,
    splitter,
    tooltip,
    image,
    status_bar,
    separator,
    edit,     // a text field; a field hosted in its own child window (native_window) is exposed by that window
    dialog,   // a modal dialog
    heading,  // a section or view title
};
const char* role_name(Role r);

// Key codes are the Windows virtual-key values, so the window passes them through unchanged.
enum Key : unsigned {
    kKeyBack = 0x08,
    kKeyTab = 0x09,
    kKeyEnter = 0x0D,
    kKeyEscape = 0x1B,
    kKeySpace = 0x20,
    kKeyPageUp = 0x21,
    kKeyPageDown = 0x22,
    kKeyEnd = 0x23,
    kKeyHome = 0x24,
    kKeyLeft = 0x25,
    kKeyUp = 0x26,
    kKeyRight = 0x27,
    kKeyDown = 0x28,
    kKeyDelete = 0x2E,
    kKeyB = 0x42,
    kKeyC = 0x43,
    kKeyE = 0x45,
    kKeyF = 0x46,
    kKeyN = 0x4E,
    kKeyU = 0x55,
    kKeyComma = 0xBC,  // VK_OEM_COMMA
    kKeySlash = 0xBF,  // VK_OEM_2 ("/" on US and most European layouts)
    kKeyApps = 0x5D,
    kKeyF10 = 0x79,
};

struct KeyEvent {
    unsigned key = 0;
    bool shift = false, ctrl = false, alt = false;
};

struct PointerEvent {
    enum Kind { down, up, move, wheel, leave } kind = move;
    float x = 0, y = 0;
    float wheel_dy = 0;  // DIPs to scroll, positive = towards the end
    int button = 0;   // 0 left, 1 right
};

class Canvas;  // drawing surface (Windows part)
class Tree;

class Node {
public:
    explicit Node(std::string automation_id = {});
    virtual ~Node() = default;
    Node(const Node&) = delete;
    Node& operator=(const Node&) = delete;

    // --- structure
    Node* add(std::unique_ptr<Node> child);
    Node* insert(size_t pos, std::unique_ptr<Node> child);
    template <class T, class... A>
    T* emplace(A&&... a) { return static_cast<T*>(add(std::make_unique<T>(std::forward<A>(a)...))); }
    std::unique_ptr<Node> remove(Node* child);
    void clear_children();
    Node* parent() const { return parent_; }
    const std::vector<std::unique_ptr<Node>>& children() const { return children_; }
    const std::string& automation_id() const { return id_; }
    uint32_t runtime_id() const { return runtime_id_; }  // unique in the process, for UI Automation
    Tree* tree() const;                                   // the tree the node is attached to, if any

    // Requests a redraw of the node's frame, or a new layout pass (size or structure changed).
    void invalidate() const;
    void relayout() const;

    // --- layout properties
    Axis axis = Axis::column;
    Sizing width = Sizing::fill, height = Sizing::fit;
    float fixed_w = 0, fixed_h = 0;  // Sizing::fixed
    float weight = 1;                // Sizing::fill
    float min_w = 0, min_h = 0;
    Insets padding;
    float gap = 0;
    // A row whose children flow onto further lines when the next one does not fit (improve-desktop-look D3): fit and
    // fixed children keep their widths, fill children take what is left of their line, and lines are `gap` apart.
    // With align_end, the children of a line without fill children sit at its end (a dialog's buttons).
    bool wraps = false;
    bool align_end = false;
    bool visible = true;
    // A leaf's content size for a given available width (wrapping text measures here). Containers measure their
    // children instead.
    std::function<Size(float avail_w)> measure;
    // A scrolling column: children keep their content heights and move by scroll_y; content_height() is their total.
    bool scrolls = false;
    // A scrolling column at its end stays at its end when its content grows (a feed following new messages).
    bool follow_end = false;
    // Outside the flow: covers the parent's whole inner rectangle, above the other children (overlays).
    bool floating = false;
    // A layer that only holds its children: the pointer passes through it where no child is (the window buttons).
    bool hit_through = false;
    float scroll_y() const { return scroll_y_; }
    float content_height() const { return content_h_; }
    float max_scroll() const;

    // --- focus and accessibility
    bool focusable = false;
    bool enabled = true;
    Role role = Role::none;
    std::u16string name;  // the accessible name (also the tooltip of icon-only controls)
    std::u16string help;  // optional longer description
    // The node is drawn and exposed by a child window of its own (a rich-edit text field): UI Automation gets it
    // from that window, so the element tree leaves it out of the accessibility tree.
    bool native_window = false;
    // The keyboard reaches what the node does through Windows instead of the Tab order (a title-bar window button:
    // the window menu, Alt+Space, and Win+Arrow keys), so the accessibility check does not require it to be focusable.
    bool system_keyboard = false;

    // Patterns (UI Automation and keyboard). -1 means the node does not support the pattern.
    virtual bool can_invoke() const { return false; }
    virtual void invoke() {}
    virtual int toggle_state() const { return -1; }  // 0 off, 1 on
    virtual void toggle() {}
    virtual int expand_state() const { return -1; }  // 0 collapsed, 1 expanded
    virtual void set_expanded(bool) {}
    virtual int selected_state() const { return -1; }  // 0 not selected, 1 selected
    virtual void select() {}
    virtual std::u16string value() const { return {}; }  // read-only text content (documents, rich text)

    // --- input: return true when handled. Keys go to the focused node, then bubble to its ancestors.
    virtual bool on_key(const KeyEvent&) { return false; }
    virtual bool on_pointer(const PointerEvent&) { return false; }
    virtual void on_hover(bool) {}
    virtual void on_focus(bool) {}

    // --- drawing (Windows part): draw() paints the node under its children, draw_over() above them.
    virtual void draw(Canvas&) {}
    virtual void draw_over(Canvas&) {}
    // Where the keyboard focus ring goes when the node has focus (a list rings its selected row).
    virtual Rect focus_rect() const { return frame_; }
    // The mouse cursor over the node: 0 arrow, 1 hand, 2 horizontal resize, 3 text.
    virtual int cursor() const { return 0; }
    bool clips = false;  // children are clipped to the frame (scrolling nodes always clip)

    // --- results
    const Rect& frame() const { return frame_; }  // absolute, in the root's coordinates

    // Called after layout gave the node its frame; elements rebuild size-dependent caches here.
    virtual void on_frame_changed() {}
    // A node may place its own children (virtual lists). It calls tree.place() for each and returns true.
    virtual bool arrange_children(Tree&, const Rect& /*inner*/) { return false; }

protected:
    // For nodes that arrange their own children (arrange_children): the scrollable content height.
    void set_content_height(float h) { content_h_ = h; }
    void set_scroll_y(float y) { scroll_y_ = y; }
    bool hovered_ = false;
    bool pressed_ = false;

private:
    friend class Tree;
    std::string id_;
    uint32_t runtime_id_;
    Node* parent_ = nullptr;
    Tree* tree_ = nullptr;  // set on the root only
    std::vector<std::unique_ptr<Node>> children_;
    Rect frame_;
    float scroll_y_ = 0, content_h_ = 0;
    // Measurement cache of the current layout pass.
    uint64_t measured_pass_ = 0;
    float measured_for_ = -1;
    Size desired_;
};

class Tree {
public:
    Tree();
    ~Tree();
    Node& root() { return *root_; }
    const Node& root() const { return *root_; }

    // Lays out the whole tree in a viewport of the given size. Nodes whose frame changed are invalidated.
    void layout(Size viewport);
    Size viewport() const { return viewport_; }
    void request_layout() {
        layout_requested_ = true;
        if (on_dirty) on_dirty();
    }
    bool layout_requested() const { return layout_requested_; }
    // Lays out again if requested (structure or size changes), keeping the viewport.
    void layout_if_needed() {
        if (layout_requested_) layout(viewport_);
    }

    // During a pass: a node whose size depends on where it was placed changed it, so the tree is laid out once more.
    void layout_again() { again_ = true; }

    // For arrange_children(): the size a child wants at an available width, and placing it.
    Size measure(Node& n, float avail_w);
    void place(Node& n, const Rect& r);

    // The deepest visible node containing the point, or nullptr outside the root.
    Node* hit_test(float x, float y) const;

    // Dirty-rectangle tracking: invalidate() marks a node's frame (or any rectangle) for redraw; take_dirty()
    // returns the merged rectangles since the last call and clears them. Overlapping rectangles merge, and more
    // than kMaxDirty rectangles collapse to their bounding box.
    static constexpr size_t kMaxDirty = 8;
    void invalidate(const Node& n) { invalidate(visible_rect(n)); }
    void invalidate(const Rect& r);
    void invalidate_all();
    std::vector<Rect> take_dirty();
    bool has_dirty() const { return !dirty_.empty(); }
    // The part of the node's frame not clipped away by scrolling or clipping ancestors.
    Rect visible_rect(const Node& n) const;

    // Scrolling: scroll_to clamps to [0, max_scroll] and moves the content; scroll_into_view scrolls every
    // scrolling ancestor so the node is visible. Both return true when something moved.
    bool scroll_to(Node& scroller, float y);
    bool scroll_into_view(const Node& n);

    // The focus chain: visible, enabled, focusable nodes in tree order (Tab order). next_focus moves forward or
    // backward and wraps; with no current node it returns the first (or last).
    std::vector<Node*> focus_chain() const;
    Node* next_focus(const Node* current, bool backward = false) const;
    Node* focused() const { return focused_; }
    void set_focus(Node* n);  // invalidates the old and the new node, scrolls it into view; nullptr clears
    Node* hovered() const { return hovered_; }
    // Clears focus, hover and capture pointers into a subtree that is being removed (Node::remove calls it).
    void forget(const Node* subtree);
    // A modal subtree (a dialog): while set, the focus chain, pointer input and keys stay inside it.
    void set_modal(Node* n);
    Node* modal() const { return modal_; }

    // Input dispatch: hover tracking, press and click (a click invokes the pressed node when released over it),
    // wheel scrolling of the nearest scrolling ancestor, Tab and Shift+Tab focus moves, Enter and Space invoke or
    // toggle the focused node, other keys go to the focused node and bubble up. Returns true when handled.
    bool pointer(const PointerEvent& e);
    bool key(const KeyEvent& e);

    // Called when the focused node changes (the window raises the UI Automation focus event).
    std::function<void(Node*)> on_focus_changed;
    // Called when invalidation happens, so the window can schedule a frame.
    std::function<void()> on_dirty;
    // Called at the start of every layout pass with the viewport, before anything is measured: responsive layouts
    // show, hide and resize panes here.
    std::function<void(Size viewport)> on_layout;

private:
    void arrange(Node& n, const Rect& r);
    void offset(Node& n, float dy);
    // Calls n.on_pointer(e); gone is set when the handler removed n (opening a Thread rebuilds the list row that
    // was clicked), after which n must not be touched.
    bool deliver(Node& n, const PointerEvent& e, bool& gone);
    std::unique_ptr<Node> root_;
    Node* delivering_ = nullptr;  // the node in on_pointer; forget() clears it when that node is removed
    uint64_t pass_ = 0;
    bool again_ = false;
    bool layout_requested_ = true;
    Node* focused_ = nullptr;
    Node* hovered_ = nullptr;
    Node* pressed_ = nullptr;
    Node* captured_ = nullptr;
    Node* modal_ = nullptr;
    Size viewport_;
    std::vector<Rect> dirty_;
};

// The accessibility tree (D8): exposed nodes (role != none, visible) under their nearest exposed ancestor.
// The root is always exposed (it is the window).
Node* a11y_parent(const Node& n);
std::vector<Node*> a11y_children(const Node& n);
// Problems that would make the window unusable with a screen reader or the keyboard: an exposed control without
// a name, a focusable node without a role, an invocable or toggleable node that the keyboard cannot reach.
std::vector<std::string> a11y_problems(const Tree& tree);

// Height bookkeeping of a virtual list (10,000 Threads; the feed): items start at an estimated height and get their
// measured height once laid out. Offsets and the index at a scroll position are O(log n).
class VirtualList {
public:
    explicit VirtualList(size_t count = 0, float estimate = 40.f) { reset(count, estimate); }
    void reset(size_t count, float estimate);
    size_t size() const { return heights_.size(); }
    void set_height(size_t i, float h);
    float height(size_t i) const { return heights_[i]; }
    float offset(size_t i) const;  // top of item i; offset(size()) is the total height
    float total() const { return offset(size()); }
    size_t index_at(float y) const;  // the item containing y (clamped to the last)
    struct Range {
        size_t first = 0, last = 0;  // [first, last)
    };
    // Items intersecting [scroll, scroll + viewport), with `overscan` extra items on each side.
    Range visible(float scroll, float viewport, size_t overscan = 2) const;

private:
    std::vector<float> heights_;
    std::vector<double> tree_;  // Fenwick tree over heights_
};

}  // namespace wb::ui
