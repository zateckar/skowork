#include "computer/uia.h"

#include "base/strings.h"
#include "platform/com.h"
#include "platform/win.h"

#include <objbase.h>
#include <oleauto.h>
#include <uiautomation.h>

#include <algorithm>
#include <cstdlib>
#include <functional>
#include <future>
#include <thread>

namespace wb::computer {

struct UiaElement {
    com_ptr<IUIAutomationElement> el;
};

struct Uia::Core {
    com_ptr<IUIAutomation> uia;
    com_ptr<IUIAutomationCacheRequest> element_cache;  // the properties of one element
    com_ptr<IUIAutomationCacheRequest> subtree_cache;  // the same over the control view of a subtree
    std::chrono::milliseconds limit{2000};
};

namespace {

// CUIAutomation8 {e22ad333-b25f-460c-83d0-0581107395c9}; MinGW declares the CLSIDs without definitions.
constexpr CLSID kCUIAutomation8 = {0xe22ad333, 0xb25f, 0x460c, {0x83, 0xd0, 0x05, 0x81, 0x10, 0x73, 0x95, 0xc9}};

struct Mta {
    HRESULT hr = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ~Mta() {
        if (SUCCEEDED(hr)) ::CoUninitialize();
    }
};

// Runs `fn` on its own MTA thread and waits at most `limit`. A call that does not finish in time is abandoned: its
// thread keeps only shared state alive and ends when the application answers.
template <typename T, typename F>
Result<T> bounded(std::chrono::milliseconds limit, F fn) {
    auto done = std::make_shared<std::promise<Result<T>>>();
    auto fut = done->get_future();
    std::thread([done, fn = std::move(fn)]() mutable {
        Mta mta;
        try {
            done->set_value(fn());
        } catch (const std::exception& e) {
            done->set_value(make_error("uia", e.what()));
        }
    }).detach();
    if (fut.wait_for(limit) != std::future_status::ready)
        return make_error("timeout", "the application did not answer UI Automation within " + std::to_string(limit.count() / 1000) + " s");
    return fut.get();
}

Status to_status(const Result<bool>& r) { return r ? Status{} : Status(r.error()); }

std::string str(BSTR b) {
    std::string s = b ? narrow(std::wstring_view(b, ::SysStringLen(b))) : std::string{};
    if (b) ::SysFreeString(b);
    return s;
}

struct Bstr {
    BSTR b = nullptr;
    explicit Bstr(const std::string& s) : b(::SysAllocString(widen(s).c_str())) {}
    ~Bstr() {
        if (b) ::SysFreeString(b);
    }
};

std::string control_type_name(CONTROLTYPEID id) {
    static const char* names[] = {"button",   "calendar",   "checkbox",  "combobox",  "edit",      "hyperlink", "image",
                                  "listitem", "list",       "menu",      "menubar",   "menuitem",  "progressbar", "radiobutton",
                                  "scrollbar", "slider",    "spinner",   "statusbar", "tab",       "tabitem",   "text",
                                  "toolbar",  "tooltip",    "tree",      "treeitem",  "custom",    "group",     "thumb",
                                  "datagrid", "dataitem",   "document",  "splitbutton", "window",  "pane",      "header",
                                  "headeritem", "table",    "titlebar",  "separator", "semanticzoom", "appbar"};
    const int i = static_cast<int>(id) - UIA_ButtonControlTypeId;
    return i >= 0 && i < static_cast<int>(std::size(names)) ? names[i] : "unknown";
}

constexpr PROPERTYID kProps[] = {
    UIA_ControlTypePropertyId, UIA_NamePropertyId, UIA_AutomationIdPropertyId, UIA_ClassNamePropertyId,
    UIA_BoundingRectanglePropertyId, UIA_IsEnabledPropertyId, UIA_IsOffscreenPropertyId, UIA_IsPasswordPropertyId,
    UIA_HasKeyboardFocusPropertyId, UIA_ValueValuePropertyId, UIA_ValueIsReadOnlyPropertyId, UIA_ToggleToggleStatePropertyId,
    UIA_SelectionItemIsSelectedPropertyId, UIA_IsInvokePatternAvailablePropertyId, UIA_IsValuePatternAvailablePropertyId,
    UIA_IsTogglePatternAvailablePropertyId, UIA_IsSelectionItemPatternAvailablePropertyId,
    UIA_IsExpandCollapsePatternAvailablePropertyId, UIA_IsScrollPatternAvailablePropertyId,
    UIA_IsScrollItemPatternAvailablePropertyId, UIA_ProcessIdPropertyId,
};

com_ptr<IUIAutomationCacheRequest> cache_request(IUIAutomation* uia, TreeScope scope) {
    com_ptr<IUIAutomationCacheRequest> cr;
    if (FAILED(uia->CreateCacheRequest(cr.put())) || !cr) return {};
    for (const PROPERTYID p : kProps) cr->AddProperty(p);
    cr->put_TreeScope(scope);
    com_ptr<IUIAutomationCondition> control_view;
    if (SUCCEEDED(uia->get_ControlViewCondition(control_view.put())) && control_view) cr->put_TreeFilter(control_view.get());
    cr->put_AutomationElementMode(AutomationElementMode_Full);
    return cr;
}

bool cached_bool(IUIAutomationElement* e, PROPERTYID id) {
    VARIANT v;
    ::VariantInit(&v);
    const bool r = SUCCEEDED(e->GetCachedPropertyValue(id, &v)) && v.vt == VT_BOOL && v.boolVal != VARIANT_FALSE;
    ::VariantClear(&v);
    return r;
}

std::string cached_string(IUIAutomationElement* e, PROPERTYID id) {
    VARIANT v;
    ::VariantInit(&v);
    std::string r;
    if (SUCCEEDED(e->GetCachedPropertyValue(id, &v)) && v.vt == VT_BSTR && v.bstrVal)
        r = narrow(std::wstring_view(v.bstrVal, ::SysStringLen(v.bstrVal)));
    ::VariantClear(&v);
    return r;
}

int cached_int(IUIAutomationElement* e, PROPERTYID id, int fallback) {
    VARIANT v;
    ::VariantInit(&v);
    int r = fallback;
    if (SUCCEEDED(e->GetCachedPropertyValue(id, &v)) && v.vt == VT_I4) r = v.lVal;
    ::VariantClear(&v);
    return r;
}

ObservedElement read_cached(IUIAutomationElement* e) {
    ObservedElement o;
    o.handle = std::make_shared<UiaElement>(UiaElement{com_ptr<IUIAutomationElement>(e)});
    CONTROLTYPEID ct = 0;
    e->get_CachedControlType(&ct);
    o.info.control_type = control_type_name(ct);
    BSTR b = nullptr;
    if (SUCCEEDED(e->get_CachedName(&b))) o.info.name = str(b);
    b = nullptr;
    if (SUCCEEDED(e->get_CachedAutomationId(&b))) o.info.automation_id = str(b);
    b = nullptr;
    if (SUCCEEDED(e->get_CachedClassName(&b))) o.info.class_name = str(b);
    o.info.is_password = cached_bool(e, UIA_IsPasswordPropertyId);
    RECT r{};
    if (SUCCEEDED(e->get_CachedBoundingRectangle(&r))) o.box = {r.left, r.top, r.right - r.left, r.bottom - r.top};
    o.enabled = cached_bool(e, UIA_IsEnabledPropertyId);
    o.focused = cached_bool(e, UIA_HasKeyboardFocusPropertyId);
    o.selected = cached_bool(e, UIA_SelectionItemIsSelectedPropertyId);
    if (cached_bool(e, UIA_IsInvokePatternAvailablePropertyId)) o.patterns |= kInvoke;
    if (cached_bool(e, UIA_IsValuePatternAvailablePropertyId)) {
        o.patterns |= kValue;
        if (cached_bool(e, UIA_ValueIsReadOnlyPropertyId)) o.patterns |= kValueReadOnly;
        // Never read back what a password field holds.
        if (!o.info.is_password) o.value = utf8_truncate(cached_string(e, UIA_ValueValuePropertyId), 400);
    }
    if (cached_bool(e, UIA_IsTogglePatternAvailablePropertyId)) {
        o.patterns |= kToggle;
        const int t = cached_int(e, UIA_ToggleToggleStatePropertyId, -1);
        o.toggle = t == ToggleState_On ? "on" : t == ToggleState_Off ? "off" : t == ToggleState_Indeterminate ? "mixed" : "";
    }
    if (cached_bool(e, UIA_IsSelectionItemPatternAvailablePropertyId)) o.patterns |= kSelectionItem;
    if (cached_bool(e, UIA_IsExpandCollapsePatternAvailablePropertyId)) o.patterns |= kExpandCollapse;
    if (cached_bool(e, UIA_IsScrollPatternAvailablePropertyId)) o.patterns |= kScroll;
    if (cached_bool(e, UIA_IsScrollItemPatternAvailablePropertyId)) o.patterns |= kScrollItem;
    return o;
}

// Containers without a name, value or pattern add nothing for the model: their children move up a level.
bool is_plain_container(const ObservedElement& o) {
    const std::string& t = o.info.control_type;
    return (t == "pane" || t == "group" || t == "custom") && o.info.name.empty() && o.patterns == 0;
}

void walk(IUIAutomationElement* e, int depth, bool root, int max, Observation& out) {
    if (static_cast<int>(out.elements.size()) >= max) {
        out.truncated = true;
        return;
    }
    ObservedElement o = read_cached(e);
    // Pruned to what is on screen (design D1); the window itself is always listed.
    if (!root && (cached_bool(e, UIA_IsOffscreenPropertyId) || o.box.empty())) return;
    int child_depth = depth;
    if (root || !is_plain_container(o)) {
        o.depth = depth;
        out.elements.push_back(std::move(o));
        child_depth = depth + 1;
    }
    if (depth > 60) return;
    com_ptr<IUIAutomationElementArray> children;
    if (FAILED(e->GetCachedChildren(children.put())) || !children) return;
    int n = 0;
    children->get_Length(&n);
    for (int i = 0; i < n; ++i) {
        com_ptr<IUIAutomationElement> c;
        if (SUCCEEDED(children->GetElement(i, c.put())) && c) walk(c.get(), child_depth, false, max, out);
    }
}

template <typename P>
Status with_pattern(const ElementHandle& e, PATTERNID id, const char* what, const std::function<HRESULT(P*)>& fn) {
    if (!e || !e->el) return make_error("stale_reference", "the element is gone");
    com_ptr<P> p;
    if (FAILED(e->el->GetCurrentPatternAs(id, __uuidof(P), p.put_void())) || !p)
        return make_error("no_pattern", std::string("the element does not support ") + what);
    const HRESULT hr = fn(p.get());
    if (hr == static_cast<HRESULT>(UIA_E_ELEMENTNOTAVAILABLE)) return make_error("stale_reference", "the element is gone");
    if (hr == static_cast<HRESULT>(UIA_E_ELEMENTNOTENABLED)) return make_error("disabled", "the element is disabled");
    if (FAILED(hr)) return make_error("uia", std::string(what) + " failed (HRESULT " + std::to_string(static_cast<long>(hr)) + ")");
    return {};
}

}  // namespace

Uia::Uia(std::shared_ptr<Core> core) : core_(std::move(core)) {}
Uia::~Uia() = default;

Result<std::shared_ptr<Uia>> Uia::create(std::chrono::milliseconds limit) {
    // The calls run on short-lived MTA threads. Without a lasting usage of the MTA, COM tears it down when the last of them
    // ends and unloads UI Automation under the interface pointers kept here and in element references.
    static const HRESULT mta = [] {
        CO_MTA_USAGE_COOKIE cookie{};
        return ::CoIncrementMTAUsage(&cookie);
    }();
    if (FAILED(mta)) return make_error("unavailable", "COM is not available");
    auto core = std::make_shared<Core>();
    core->limit = limit;
    auto made = bounded<bool>(std::chrono::seconds(10), [core]() -> Result<bool> {
        if (FAILED(::CoCreateInstance(kCUIAutomation8, nullptr, CLSCTX_INPROC_SERVER, __uuidof(IUIAutomation), core->uia.put_void())) ||
            !core->uia)
            return make_error("unavailable", "UI Automation is not available");
        core->element_cache = cache_request(core->uia.get(), TreeScope_Element);
        core->subtree_cache = cache_request(core->uia.get(), TreeScope_Subtree);
        if (!core->element_cache || !core->subtree_cache) return make_error("unavailable", "UI Automation cache requests failed");
        return true;
    });
    if (!made) return made.error();
    return std::shared_ptr<Uia>(new Uia(std::move(core)));
}

Result<Observation> Uia::observe(HWND hwnd, int max_elements) {
    auto core = core_;
    return bounded<Observation>(core->limit, [core, hwnd, max_elements]() -> Result<Observation> {
        com_ptr<IUIAutomationElement> root;
        if (FAILED(core->uia->ElementFromHandle(hwnd, root.put())) || !root) return make_error("window_gone", "the window is not available");
        com_ptr<IUIAutomationElement> cached;
        // The tree may change while it is read (a list closing); one retry covers that.
        for (int attempt = 0; attempt < 3 && !cached; ++attempt) {
            if (attempt > 0) std::this_thread::sleep_for(std::chrono::milliseconds(50));
            if (FAILED(root->BuildUpdatedCache(core->subtree_cache.get(), cached.put()))) cached = nullptr;
        }
        if (!cached) return make_error("uia", "the window's elements could not be read");
        Observation out;
        walk(cached.get(), 0, true, max_elements, out);
        return out;
    });
}

Result<ObservedElement> Uia::facts(const ElementHandle& e) {
    if (!e || !e->el) return make_error("stale_reference", "the element is gone");
    auto core = core_;
    return bounded<ObservedElement>(core->limit, [core, e]() -> Result<ObservedElement> {
        com_ptr<IUIAutomationElement> cached;
        if (FAILED(e->el->BuildUpdatedCache(core->element_cache.get(), cached.put())) || !cached)
            return make_error("stale_reference", "the element is gone; observe the window again");
        ObservedElement o = read_cached(cached.get());
        o.handle = e;
        return o;
    });
}

Result<std::pair<ObservedElement, DWORD>> Uia::element_at(int x, int y) {
    auto core = core_;
    return bounded<std::pair<ObservedElement, DWORD>>(core->limit, [core, x, y]() -> Result<std::pair<ObservedElement, DWORD>> {
        com_ptr<IUIAutomationElement> el;
        if (FAILED(core->uia->ElementFromPoint(POINT{x, y}, el.put())) || !el) return make_error("uia", "no element at that point");
        com_ptr<IUIAutomationElement> cached;
        if (FAILED(el->BuildUpdatedCache(core->element_cache.get(), cached.put())) || !cached) return make_error("uia", "the element could not be read");
        int pid = 0;
        cached->get_CachedProcessId(&pid);
        return std::make_pair(read_cached(cached.get()), static_cast<DWORD>(pid));
    });
}

Result<std::pair<ObservedElement, DWORD>> Uia::focused() {
    auto core = core_;
    return bounded<std::pair<ObservedElement, DWORD>>(core->limit, [core]() -> Result<std::pair<ObservedElement, DWORD>> {
        com_ptr<IUIAutomationElement> el;
        if (FAILED(core->uia->GetFocusedElement(el.put())) || !el) return make_error("uia", "no element has the keyboard focus");
        com_ptr<IUIAutomationElement> cached;
        if (FAILED(el->BuildUpdatedCache(core->element_cache.get(), cached.put())) || !cached) return make_error("uia", "the element could not be read");
        int pid = 0;
        cached->get_CachedProcessId(&pid);
        return std::make_pair(read_cached(cached.get()), static_cast<DWORD>(pid));
    });
}

Result<HWND> Uia::window_of(const ElementHandle& e) {
    if (!e || !e->el) return make_error("stale_reference", "the element is gone");
    auto core = core_;
    return bounded<HWND>(core->limit, [core, e]() -> Result<HWND> {
        com_ptr<IUIAutomationTreeWalker> walker;
        core->uia->get_RawViewWalker(walker.put());
        com_ptr<IUIAutomationElement> cur = e->el;
        for (int i = 0; cur && i < 100; ++i) {
            UIA_HWND h = nullptr;
            if (SUCCEEDED(cur->get_CurrentNativeWindowHandle(&h)) && h) return ::GetAncestor(static_cast<HWND>(h), GA_ROOT);
            com_ptr<IUIAutomationElement> parent;
            if (!walker || FAILED(walker->GetParentElement(cur.get(), parent.put()))) break;
            cur = parent;
        }
        return make_error("uia", "the element's window is unknown");
    });
}

Status Uia::invoke(const ElementHandle& e) {
    return to_status(bounded<bool>(core_->limit, [e]() -> Result<bool> {
        if (Status s = with_pattern<IUIAutomationInvokePattern>(e, UIA_InvokePatternId, "invoke", [](auto* p) { return p->Invoke(); }); !s) return s.error();
        return true;
    }));
}

Status Uia::toggle(const ElementHandle& e) {
    return to_status(bounded<bool>(core_->limit, [e]() -> Result<bool> {
        if (Status s = with_pattern<IUIAutomationTogglePattern>(e, UIA_TogglePatternId, "toggle", [](auto* p) { return p->Toggle(); }); !s) return s.error();
        return true;
    }));
}

Status Uia::select_item(const ElementHandle& e) {
    return to_status(bounded<bool>(core_->limit, [e]() -> Result<bool> {
        if (Status s = with_pattern<IUIAutomationSelectionItemPattern>(e, UIA_SelectionItemPatternId, "selection", [](auto* p) { return p->Select(); }); !s)
            return s.error();
        return true;
    }));
}

Status Uia::set_value(const ElementHandle& e, const std::string& text) {
    return to_status(bounded<bool>(core_->limit, [e, text]() -> Result<bool> {
        Bstr b(text);
        if (Status s = with_pattern<IUIAutomationValuePattern>(e, UIA_ValuePatternId, "value", [&b](auto* p) { return p->SetValue(b.b); }); !s)
            return s.error();
        return true;
    }));
}

Status Uia::set_focus(const ElementHandle& e) {
    if (!e || !e->el) return make_error("stale_reference", "the element is gone");
    return to_status(bounded<bool>(core_->limit, [e]() -> Result<bool> {
        if (FAILED(e->el->SetFocus())) return make_error("uia", "the element cannot take the keyboard focus");
        return true;
    }));
}

Status Uia::expand(const ElementHandle& e) {
    return to_status(bounded<bool>(core_->limit, [e]() -> Result<bool> {
        if (Status s = with_pattern<IUIAutomationExpandCollapsePattern>(e, UIA_ExpandCollapsePatternId, "expand", [](auto* p) { return p->Expand(); }); !s)
            return s.error();
        return true;
    }));
}

Status Uia::collapse(const ElementHandle& e) {
    return to_status(bounded<bool>(core_->limit, [e]() -> Result<bool> {
        if (Status s = with_pattern<IUIAutomationExpandCollapsePattern>(e, UIA_ExpandCollapsePatternId, "collapse", [](auto* p) { return p->Collapse(); }); !s)
            return s.error();
        return true;
    }));
}

Status Uia::scroll(const ElementHandle& e, int vertical, int horizontal) {
    return to_status(bounded<bool>(core_->limit, [e, vertical, horizontal]() -> Result<bool> {
        const auto amount = [](int n) { return n < 0 ? ScrollAmount_LargeDecrement : n > 0 ? ScrollAmount_LargeIncrement : ScrollAmount_NoAmount; };
        const int steps = std::max(std::abs(vertical), std::abs(horizontal));
        for (int i = 0; i < steps; ++i) {
            const ScrollAmount h = i < std::abs(horizontal) ? amount(horizontal) : ScrollAmount_NoAmount;
            const ScrollAmount v = i < std::abs(vertical) ? amount(vertical) : ScrollAmount_NoAmount;
            if (Status s = with_pattern<IUIAutomationScrollPattern>(e, UIA_ScrollPatternId, "scroll", [h, v](auto* p) { return p->Scroll(h, v); }); !s)
                return s.error();
        }
        return true;
    }));
}

Status Uia::scroll_into_view(const ElementHandle& e) {
    return to_status(bounded<bool>(core_->limit, [e]() -> Result<bool> {
        if (Status s = with_pattern<IUIAutomationScrollItemPattern>(e, UIA_ScrollItemPatternId, "scroll into view", [](auto* p) { return p->ScrollIntoView(); }); !s)
            return s.error();
        return true;
    }));
}

Result<ElementHandle> Uia::find_option(const ElementHandle& container, const std::string& name) {
    if (!container || !container->el) return make_error("stale_reference", "the element is gone");
    auto core = core_;
    return bounded<ElementHandle>(core->limit, [core, container, name]() -> Result<ElementHandle> {
        com_ptr<IUIAutomationCondition> all;
        core->uia->CreateTrueCondition(all.put());
        com_ptr<IUIAutomationElementArray> found;
        if (FAILED(container->el->FindAll(TreeScope_Descendants, all.get(), found.put())) || !found)
            return make_error("uia", "the options could not be read");
        const std::string want = fold_latin(trim(name));
        int n = 0;
        found->get_Length(&n);
        ElementHandle fallback;
        for (int i = 0; i < n; ++i) {
            com_ptr<IUIAutomationElement> c;
            if (FAILED(found->GetElement(i, c.put())) || !c) continue;
            BSTR b = nullptr;
            c->get_CurrentName(&b);
            if (fold_latin(trim(str(b))) != want) continue;
            com_ptr<IUIAutomationSelectionItemPattern> p;
            auto h = std::make_shared<UiaElement>(UiaElement{c});
            if (SUCCEEDED(c->GetCurrentPatternAs(UIA_SelectionItemPatternId, __uuidof(IUIAutomationSelectionItemPattern), p.put_void())) && p) return h;
            if (!fallback) fallback = h;
        }
        if (fallback) return fallback;
        return make_error("no_option", "no option named \"" + name + "\"");
    });
}

}  // namespace wb::computer
