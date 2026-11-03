// UI Automation for computer use (client/computer-use Accessibility Tree Perception and Element-First Actions;
// add-computer-use design D1): one cache request per observe fetches the window's control view in a single
// cross-process round trip; actions go through the element's patterns. Every call runs on its own MTA thread and is
// abandoned after a time limit, so a hung application yields an error instead of a hung Run.
#pragma once

#include "base/result.h"
#include "computer/classify.h"

#include <windows.h>

#include <chrono>
#include <memory>
#include <string>
#include <vector>

namespace wb::computer {

struct Box {
    int x = 0, y = 0, w = 0, h = 0;  // physical screen pixels
    [[nodiscard]] bool empty() const { return w <= 0 || h <= 0; }
    [[nodiscard]] int cx() const { return x + w / 2; }
    [[nodiscard]] int cy() const { return y + h / 2; }
};

// Patterns an element offers (bit flags).
enum PatternFlag : unsigned {
    kInvoke = 1,
    kValue = 2,
    kToggle = 4,
    kSelectionItem = 8,
    kExpandCollapse = 16,
    kScroll = 32,
    kValueReadOnly = 64,
    kScrollItem = 128,
};

struct UiaElement;  // a live UI Automation element (free-threaded)
using ElementHandle = std::shared_ptr<UiaElement>;

struct ObservedElement {
    ElementHandle handle;
    int depth = 0;
    ElementInfo info;
    std::string value;
    std::string toggle;  // on | off | mixed, for toggles
    Box box;
    unsigned patterns = 0;
    bool enabled = true;
    bool focused = false;
    bool selected = false;
};

struct Observation {
    std::vector<ObservedElement> elements;  // depth-first, the window first
    bool truncated = false;                 // more elements than the limit
};

class Uia {
public:
    // Fails when UI Automation is not available.
    static Result<std::shared_ptr<Uia>> create(std::chrono::milliseconds limit = std::chrono::seconds(2));
    ~Uia();

    // The window's control view, pruned to elements on screen, at most `max_elements`.
    Result<Observation> observe(HWND hwnd, int max_elements);
    // Current facts of an element (the observation may be stale).
    Result<ObservedElement> facts(const ElementHandle& e);
    // The element at a screen point and its process id.
    Result<std::pair<ObservedElement, DWORD>> element_at(int x, int y);
    // The element with keyboard focus and its process id.
    Result<std::pair<ObservedElement, DWORD>> focused();
    // The native window an element belongs to (its nearest ancestor with a window handle).
    Result<HWND> window_of(const ElementHandle& e);

    Status invoke(const ElementHandle& e);
    Status toggle(const ElementHandle& e);
    Status select_item(const ElementHandle& e);
    Status set_value(const ElementHandle& e, const std::string& text);
    Status set_focus(const ElementHandle& e);
    Status expand(const ElementHandle& e);
    Status collapse(const ElementHandle& e);
    // Scrolls a scrollable element: `vertical` / `horizontal` in steps of a page (negative: up / left).
    Status scroll(const ElementHandle& e, int vertical, int horizontal);
    Status scroll_into_view(const ElementHandle& e);
    // A descendant whose name or value equals `name` (case-insensitive) and that can be selected.
    Result<ElementHandle> find_option(const ElementHandle& container, const std::string& name);

private:
    struct Core;
    explicit Uia(std::shared_ptr<Core> core);
    std::shared_ptr<Core> core_;
};

}  // namespace wb::computer
