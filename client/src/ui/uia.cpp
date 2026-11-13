#include "ui/uia.h"

#include <ole2.h>
#include <uiautomation.h>

#include <algorithm>

#ifndef UIA_ScrollPatternNoScroll
#define UIA_ScrollPatternNoScroll (-1.0)
#endif

namespace wb::ui {

namespace {
// Windows 10 1803+ properties, not in every SDK's headers.
constexpr PROPERTYID kHeadingLevelPropertyId = 30173;  // UIA_HeadingLevelPropertyId
constexpr PROPERTYID kIsDialogPropertyId = 30174;      // UIA_IsDialogPropertyId
constexpr long kHeadingLevel1 = 80051;                 // HeadingLevel1
}  // namespace

struct Accessibility::State {
    UiaSite* site = nullptr;
    DWORD thread = ::GetCurrentThreadId();  // the window's thread, which creates the Accessibility
    HWND hwnd = nullptr;                    // set when UI Automation first asks for the window's provider
};

bool Accessibility::run_call(WPARAM wp, LPARAM lp) {
    if (wp != kUiaCallMagic || !lp) return false;
    (*reinterpret_cast<const std::function<void()>*>(lp))();
    return true;
}

long control_type_of(Role r) {
    switch (r) {
    case Role::none:
    case Role::pane:
    case Role::scroll_view: return UIA_PaneControlTypeId;
    case Role::group: return UIA_GroupControlTypeId;
    case Role::text: return UIA_TextControlTypeId;
    case Role::document: return UIA_DocumentControlTypeId;
    case Role::button: return UIA_ButtonControlTypeId;
    case Role::checkbox: return UIA_CheckBoxControlTypeId;
    case Role::link: return UIA_HyperlinkControlTypeId;
    case Role::list: return UIA_ListControlTypeId;
    case Role::list_item: return UIA_ListItemControlTypeId;
    case Role::menu: return UIA_MenuControlTypeId;
    case Role::menu_item: return UIA_MenuItemControlTypeId;
    case Role::splitter: return UIA_ThumbControlTypeId;
    case Role::tooltip: return UIA_ToolTipControlTypeId;
    case Role::image: return UIA_ImageControlTypeId;
    case Role::status_bar: return UIA_StatusBarControlTypeId;
    case Role::separator: return UIA_SeparatorControlTypeId;
    case Role::edit: return UIA_EditControlTypeId;
    case Role::dialog: return UIA_WindowControlTypeId;
    case Role::heading: return UIA_TextControlTypeId;
    }
    return UIA_PaneControlTypeId;
}

namespace {

constexpr HRESULT kNotAvailable = static_cast<HRESULT>(UIA_E_ELEMENTNOTAVAILABLE);

Node* find(Node& n, uint32_t id) {
    if (n.runtime_id() == id) return &n;
    for (const auto& c : n.children())
        if (Node* f = find(*c, id)) return f;
    return nullptr;
}

HRESULT bstr(VARIANT* v, std::u16string_view s) {
    v->vt = VT_BSTR;
    v->bstrVal = ::SysAllocStringLen(reinterpret_cast<const OLECHAR*>(s.data()), static_cast<UINT>(s.size()));
    return v->bstrVal ? S_OK : E_OUTOFMEMORY;
}

HRESULT bstr(VARIANT* v, const std::string& s) { return bstr(v, std::u16string(s.begin(), s.end())); }

HRESULT boolean(VARIANT* v, bool b) {
    v->vt = VT_BOOL;
    v->boolVal = b ? VARIANT_TRUE : VARIANT_FALSE;
    return S_OK;
}

class Provider final : public IRawElementProviderSimple,
                       public IRawElementProviderFragment,
                       public IRawElementProviderFragmentRoot,
                       public IInvokeProvider,
                       public IToggleProvider,
                       public IExpandCollapseProvider,
                       public ISelectionItemProvider,
                       public ISelectionProvider,
                       public IScrollProvider,
                       public IValueProvider {
public:
    // id 0 is the window (the fragment root); other ids are node runtime ids.
    Provider(std::shared_ptr<Accessibility::State> s, uint32_t id) : s_(std::move(s)), id_(id) {}

    static Provider* of(const std::shared_ptr<Accessibility::State>& s, Node& n) {
        Tree* t = n.tree();
        const bool root = t && &n == &t->root();
        return new Provider(s, root ? 0 : n.runtime_id());
    }

    // IUnknown
    ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(::InterlockedIncrement(&refs_)); }
    ULONG STDMETHODCALLTYPE Release() override {
        const LONG n = ::InterlockedDecrement(&refs_);
        if (n == 0) delete this;
        return static_cast<ULONG>(n);
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IRawElementProviderSimple)) *out = static_cast<IRawElementProviderSimple*>(this);
        else if (riid == __uuidof(IRawElementProviderFragment)) *out = static_cast<IRawElementProviderFragment*>(this);
        else if (riid == __uuidof(IRawElementProviderFragmentRoot) && id_ == 0) *out = static_cast<IRawElementProviderFragmentRoot*>(this);
        else if (pattern_ptr(riid)) {
            bool ok = false;
            (void)on_ui([&]() -> HRESULT {
                Node* n = node();
                ok = n && supports(*n, riid);
                return S_OK;
            });
            if (ok) *out = pattern_ptr(riid);
        }
        if (!*out) return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }

    // IRawElementProviderSimple
    HRESULT STDMETHODCALLTYPE get_ProviderOptions(enum ProviderOptions* ret) override {
        *ret = ProviderOptions_ServerSideProvider;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPatternProvider(PATTERNID pattern, IUnknown** ret) override {
        *ret = nullptr;
        return on_ui([&]() -> HRESULT {
            if (!alive()) return kNotAvailable;
            Node* n = node();
            if (!n || id_ == 0) return S_OK;
            const IID* iid = pattern_iid(pattern);
            if (iid && supports(*n, *iid)) {
                *ret = static_cast<IRawElementProviderSimple*>(this);
                AddRef();
            }
            return S_OK;
        });
    }
    HRESULT STDMETHODCALLTYPE GetPropertyValue(PROPERTYID prop, VARIANT* ret) override {
        ::VariantInit(ret);
        return on_ui([&]() -> HRESULT {
            if (!alive()) return kNotAvailable;
            if (id_ == 0) {
                if (prop == UIA_FrameworkIdPropertyId) return bstr(ret, std::u16string_view(u"Skowork"));
                return S_OK;  // the HWND host provider supplies the window's name, type and bounds
            }
            Node* n = node();
            if (!n) return kNotAvailable;
            switch (prop) {
            case UIA_ControlTypePropertyId:
                ret->vt = VT_I4;
                ret->lVal = control_type_of(n->role);
                return S_OK;
            case UIA_NamePropertyId: return bstr(ret, std::u16string_view(n->name));
            case UIA_HelpTextPropertyId: return n->help.empty() ? S_OK : bstr(ret, std::u16string_view(n->help));
            case UIA_AutomationIdPropertyId: return bstr(ret, n->automation_id());
            case UIA_FrameworkIdPropertyId: return bstr(ret, std::u16string_view(u"Skowork"));
            case UIA_IsKeyboardFocusablePropertyId: return boolean(ret, n->focusable && n->enabled);
            case UIA_HasKeyboardFocusPropertyId: return boolean(ret, s_->site->focus_target() == n && ::GetFocus() == s_->site->hwnd());
            case UIA_IsEnabledPropertyId: return boolean(ret, n->enabled);
            case UIA_IsOffscreenPropertyId: return boolean(ret, s_->site->tree().visible_rect(*n).empty());
            case UIA_IsControlElementPropertyId: return boolean(ret, true);
            case UIA_IsContentElementPropertyId: return boolean(ret, n->role != Role::separator && n->role != Role::tooltip);
            case kHeadingLevelPropertyId:
                if (n->role != Role::heading) return S_OK;
                ret->vt = VT_I4;
                ret->lVal = kHeadingLevel1;
                return S_OK;
            case kIsDialogPropertyId: return n->role == Role::dialog ? boolean(ret, true) : S_OK;
            default: return S_OK;
            }
        });
    }
    HRESULT STDMETHODCALLTYPE get_HostRawElementProvider(IRawElementProviderSimple** ret) override {
        *ret = nullptr;
        return on_ui([&]() -> HRESULT {
            if (id_ != 0) return S_OK;
            if (!alive()) return kNotAvailable;
            return ::UiaHostProviderFromHwnd(s_->site->hwnd(), ret);
        });
    }

    // IRawElementProviderFragment
    HRESULT STDMETHODCALLTYPE Navigate(enum NavigateDirection dir, IRawElementProviderFragment** ret) override {
        *ret = nullptr;
        return on_ui([&]() -> HRESULT {
            if (!alive()) return kNotAvailable;
            Node* n = node();
            if (!n) return kNotAvailable;
            Node* target = nullptr;
            if (dir == NavigateDirection_Parent) {
                if (id_ == 0) return S_OK;
                target = a11y_parent(*n);
            } else if (dir == NavigateDirection_FirstChild || dir == NavigateDirection_LastChild) {
                const auto kids = a11y_children(*n);
                if (!kids.empty()) target = dir == NavigateDirection_FirstChild ? kids.front() : kids.back();
            } else {
                if (id_ == 0) return S_OK;
                Node* p = a11y_parent(*n);
                if (!p) return S_OK;
                const auto sib = a11y_children(*p);
                const auto it = std::find(sib.begin(), sib.end(), n);
                if (it == sib.end()) return S_OK;
                if (dir == NavigateDirection_NextSibling && it + 1 != sib.end()) target = *(it + 1);
                if (dir == NavigateDirection_PreviousSibling && it != sib.begin()) target = *(it - 1);
            }
            if (target) *ret = static_cast<IRawElementProviderFragment*>(of(s_, *target));
            return S_OK;
        });
    }
    HRESULT STDMETHODCALLTYPE GetRuntimeId(SAFEARRAY** ret) override {
        *ret = nullptr;
        return on_ui([&]() -> HRESULT {
            if (!alive()) return kNotAvailable;
            if (id_ == 0) return S_OK;  // the host provider supplies the window's id
            int ids[2] = {UiaAppendRuntimeId, static_cast<int>(id_)};
            SAFEARRAY* a = ::SafeArrayCreateVector(VT_I4, 0, 2);
            if (!a) return E_OUTOFMEMORY;
            for (LONG i = 0; i < 2; ++i) ::SafeArrayPutElement(a, &i, &ids[i]);
            *ret = a;
            return S_OK;
        });
    }
    HRESULT STDMETHODCALLTYPE get_BoundingRectangle(struct UiaRect* ret) override {
        *ret = UiaRect{};
        return on_ui([&]() -> HRESULT {
            if (!alive()) return kNotAvailable;
            if (id_ == 0) return S_OK;
            Node* n = node();
            if (!n) return kNotAvailable;
            const Rect v = s_->site->tree().visible_rect(*n);
            if (v.empty()) return S_OK;
            const RECT r = s_->site->screen_rect(v);
            *ret = UiaRect{static_cast<double>(r.left), static_cast<double>(r.top), static_cast<double>(r.right - r.left),
                           static_cast<double>(r.bottom - r.top)};
            return S_OK;
        });
    }
    HRESULT STDMETHODCALLTYPE GetEmbeddedFragmentRoots(SAFEARRAY** ret) override {
        *ret = nullptr;
        return on_ui([&]() -> HRESULT {
            return S_OK;
        });
    }
    HRESULT STDMETHODCALLTYPE SetFocus() override {
        return on_ui([&]() -> HRESULT {
            if (!alive()) return kNotAvailable;
            Node* n = node();
            if (!n) return kNotAvailable;
            // A row takes focus through its list.
            if (!n->focusable && n->selected_state() >= 0) n->select();
            for (Node* a = n; a; a = a->parent())
                if (a->focusable) {
                    s_->site->focus(*a);
                    break;
                }
            return S_OK;
        });
    }
    HRESULT STDMETHODCALLTYPE get_FragmentRoot(IRawElementProviderFragmentRoot** ret) override {
        *ret = nullptr;
        return on_ui([&]() -> HRESULT {
            if (!alive()) return kNotAvailable;
            *ret = static_cast<IRawElementProviderFragmentRoot*>(new Provider(s_, 0));
            return S_OK;
        });
    }

    // IRawElementProviderFragmentRoot
    HRESULT STDMETHODCALLTYPE ElementProviderFromPoint(double x, double y, IRawElementProviderFragment** ret) override {
        *ret = nullptr;
        return on_ui([&]() -> HRESULT {
            if (!alive()) return kNotAvailable;
            Node* h = s_->site->node_at_screen(POINT{static_cast<LONG>(x), static_cast<LONG>(y)});
            while (h && h->parent() && h->role == Role::none) h = h->parent();
            if (h) *ret = static_cast<IRawElementProviderFragment*>(of(s_, *h));
            return S_OK;
        });
    }
    HRESULT STDMETHODCALLTYPE GetFocus(IRawElementProviderFragment** ret) override {
        *ret = nullptr;
        return on_ui([&]() -> HRESULT {
            if (!alive()) return kNotAvailable;
            if (Node* f = s_->site->focus_target()) *ret = static_cast<IRawElementProviderFragment*>(of(s_, *f));
            return S_OK;
        });
    }

    // IInvokeProvider
    HRESULT STDMETHODCALLTYPE Invoke() override {
        return on_ui([&]() -> HRESULT {
            Node* n = live_node();
            if (!n) return kNotAvailable;
            if (!n->enabled) return static_cast<HRESULT>(UIA_E_ELEMENTNOTENABLED);
            ::UiaRaiseAutomationEvent(static_cast<IRawElementProviderSimple*>(this), UIA_Invoke_InvokedEventId);
            n->invoke();  // may destroy the node (a menu item closes its menu)
            return S_OK;
        });
    }

    // IToggleProvider
    HRESULT STDMETHODCALLTYPE Toggle() override {
        return on_ui([&]() -> HRESULT {
            Node* n = live_node();
            if (!n) return kNotAvailable;
            if (!n->enabled) return static_cast<HRESULT>(UIA_E_ELEMENTNOTENABLED);
            n->toggle();
            return S_OK;
        });
    }
    HRESULT STDMETHODCALLTYPE get_ToggleState(enum ToggleState* ret) override {
        return on_ui([&]() -> HRESULT {
            Node* n = live_node();
            if (!n) return kNotAvailable;
            *ret = n->toggle_state() == 1 ? ToggleState_On : ToggleState_Off;
            return S_OK;
        });
    }

    // IExpandCollapseProvider
    HRESULT STDMETHODCALLTYPE Expand() override { return set_expanded(true); }
    HRESULT STDMETHODCALLTYPE Collapse() override { return set_expanded(false); }
    HRESULT STDMETHODCALLTYPE get_ExpandCollapseState(enum ExpandCollapseState* ret) override {
        return on_ui([&]() -> HRESULT {
            Node* n = live_node();
            if (!n) return kNotAvailable;
            *ret = n->expand_state() == 1 ? ExpandCollapseState_Expanded : ExpandCollapseState_Collapsed;
            return S_OK;
        });
    }

    // ISelectionItemProvider
    HRESULT STDMETHODCALLTYPE Select() override {
        return on_ui([&]() -> HRESULT {
            Node* n = live_node();
            if (!n) return kNotAvailable;
            n->select();
            ::UiaRaiseAutomationEvent(static_cast<IRawElementProviderSimple*>(this), UIA_SelectionItem_ElementSelectedEventId);
            return S_OK;
        });
    }
    HRESULT STDMETHODCALLTYPE AddToSelection() override { return Select(); }
    HRESULT STDMETHODCALLTYPE RemoveFromSelection() override { return static_cast<HRESULT>(UIA_E_INVALIDOPERATION); }
    HRESULT STDMETHODCALLTYPE get_IsSelected(BOOL* ret) override {
        return on_ui([&]() -> HRESULT {
            Node* n = live_node();
            if (!n) return kNotAvailable;
            *ret = n->selected_state() == 1 ? TRUE : FALSE;
            return S_OK;
        });
    }
    HRESULT STDMETHODCALLTYPE get_SelectionContainer(IRawElementProviderSimple** ret) override {
        *ret = nullptr;
        return on_ui([&]() -> HRESULT {
            Node* n = live_node();
            if (!n) return kNotAvailable;
            if (Node* p = a11y_parent(*n)) *ret = static_cast<IRawElementProviderSimple*>(of(s_, *p));
            return S_OK;
        });
    }

    // ISelectionProvider (lists)
    HRESULT STDMETHODCALLTYPE GetSelection(SAFEARRAY** ret) override {
        *ret = nullptr;
        return on_ui([&]() -> HRESULT {
            Node* n = live_node();
            if (!n) return kNotAvailable;
            std::vector<Node*> sel;
            for (Node* c : a11y_children(*n))
                if (c->selected_state() == 1) sel.push_back(c);
            SAFEARRAY* a = ::SafeArrayCreateVector(VT_UNKNOWN, 0, static_cast<ULONG>(sel.size()));
            if (!a) return E_OUTOFMEMORY;
            for (LONG i = 0; i < static_cast<LONG>(sel.size()); ++i) {
                IRawElementProviderSimple* p = of(s_, *sel[static_cast<size_t>(i)]);
                ::SafeArrayPutElement(a, &i, static_cast<IUnknown*>(p));  // adds its own reference
                p->Release();
            }
            *ret = a;
            return S_OK;
        });
    }
    HRESULT STDMETHODCALLTYPE get_CanSelectMultiple(BOOL* ret) override {
        *ret = FALSE;
        return on_ui([&]() -> HRESULT {
            return alive() ? S_OK : kNotAvailable;
        });
    }
    HRESULT STDMETHODCALLTYPE get_IsSelectionRequired(BOOL* ret) override {
        *ret = FALSE;
        return on_ui([&]() -> HRESULT {
            return alive() ? S_OK : kNotAvailable;
        });
    }

    // IScrollProvider (vertical only)
    HRESULT STDMETHODCALLTYPE Scroll(enum ScrollAmount h, enum ScrollAmount v) override {
        return on_ui([&]() -> HRESULT {
            Node* n = live_node();
            if (!n) return kNotAvailable;
            if (h != ScrollAmount_NoAmount) return static_cast<HRESULT>(UIA_E_INVALIDOPERATION);
            const float page = std::max(20.f, n->frame().h - 40.f), line = 48.f;
            float d = 0;
            switch (v) {
            case ScrollAmount_LargeDecrement: d = -page; break;
            case ScrollAmount_SmallDecrement: d = -line; break;
            case ScrollAmount_LargeIncrement: d = page; break;
            case ScrollAmount_SmallIncrement: d = line; break;
            default: break;
            }
            s_->site->tree().scroll_to(*n, n->scroll_y() + d);
            return S_OK;
        });
    }
    HRESULT STDMETHODCALLTYPE SetScrollPercent(double h, double v) override {
        return on_ui([&]() -> HRESULT {
            Node* n = live_node();
            if (!n) return kNotAvailable;
            if (h != UIA_ScrollPatternNoScroll && h != 0) return static_cast<HRESULT>(UIA_E_INVALIDOPERATION);
            if (v != UIA_ScrollPatternNoScroll) {
                if (v < 0 || v > 100) return E_INVALIDARG;
                s_->site->tree().scroll_to(*n, static_cast<float>(v / 100.0) * n->max_scroll());
            }
            return S_OK;
        });
    }
    HRESULT STDMETHODCALLTYPE get_HorizontalScrollPercent(double* ret) override {
        *ret = UIA_ScrollPatternNoScroll;
        return on_ui([&]() -> HRESULT {
            return alive() ? S_OK : kNotAvailable;
        });
    }
    HRESULT STDMETHODCALLTYPE get_VerticalScrollPercent(double* ret) override {
        return on_ui([&]() -> HRESULT {
            Node* n = live_node();
            if (!n) return kNotAvailable;
            *ret = n->max_scroll() > 0 ? 100.0 * n->scroll_y() / n->max_scroll() : UIA_ScrollPatternNoScroll;
            return S_OK;
        });
    }
    HRESULT STDMETHODCALLTYPE get_HorizontalViewSize(double* ret) override {
        *ret = 100.0;
        return on_ui([&]() -> HRESULT {
            return alive() ? S_OK : kNotAvailable;
        });
    }
    HRESULT STDMETHODCALLTYPE get_VerticalViewSize(double* ret) override {
        return on_ui([&]() -> HRESULT {
            Node* n = live_node();
            if (!n) return kNotAvailable;
            *ret = n->content_height() > 0 ? std::min(100.0, 100.0 * n->frame().h / n->content_height()) : 100.0;
            return S_OK;
        });
    }
    HRESULT STDMETHODCALLTYPE get_HorizontallyScrollable(BOOL* ret) override {
        *ret = FALSE;
        return on_ui([&]() -> HRESULT {
            return alive() ? S_OK : kNotAvailable;
        });
    }
    HRESULT STDMETHODCALLTYPE get_VerticallyScrollable(BOOL* ret) override {
        return on_ui([&]() -> HRESULT {
            Node* n = live_node();
            if (!n) return kNotAvailable;
            *ret = n->max_scroll() > 0 ? TRUE : FALSE;
            return S_OK;
        });
    }

    // IValueProvider (read-only text of documents)
    HRESULT STDMETHODCALLTYPE SetValue(LPCWSTR) override { return static_cast<HRESULT>(UIA_E_ELEMENTNOTENABLED); }
    HRESULT STDMETHODCALLTYPE get_Value(BSTR* ret) override {
        *ret = nullptr;
        return on_ui([&]() -> HRESULT {
            Node* n = live_node();
            if (!n) return kNotAvailable;
            const std::u16string v = n->value();
            *ret = ::SysAllocStringLen(reinterpret_cast<const OLECHAR*>(v.data()), static_cast<UINT>(v.size()));
            return *ret ? S_OK : E_OUTOFMEMORY;
        });
    }
    HRESULT STDMETHODCALLTYPE get_IsReadOnly(BOOL* ret) override {
        *ret = TRUE;
        return on_ui([&]() -> HRESULT {
            return alive() ? S_OK : kNotAvailable;
        });
    }

private:
    ~Provider() = default;

    // UI Automation calls providers on its own threads, and the element tree belongs to the window's thread: every
    // call that reads or changes the tree runs there. A call on the window's thread (an event the window raises) runs
    // at once; any other is sent to the window, which runs it before it handles anything else. Without the window
    // (closed) the element is not available.
    template <class F>
    HRESULT on_ui(F&& f) {
        if (::GetCurrentThreadId() == s_->thread) return f();
        const HWND h = s_->hwnd;
        if (!h) return kNotAvailable;
        HRESULT hr = kNotAvailable;
        const std::function<void()> call = [&] { hr = f(); };
        if (!::SendMessageW(h, kUiaCallMessage, kUiaCallMagic, reinterpret_cast<LPARAM>(&call))) return kNotAvailable;
        return hr;
    }

    bool alive() const { return s_->site != nullptr; }
    Node* node() const {
        if (!alive()) return nullptr;
        Node& root = s_->site->tree().root();
        return id_ == 0 ? &root : find(root, id_);
    }
    Node* live_node() const { return id_ == 0 ? nullptr : node(); }

    static bool supports(const Node& n, REFIID iid) {
        if (iid == __uuidof(IInvokeProvider)) return n.can_invoke();
        if (iid == __uuidof(IToggleProvider)) return n.toggle_state() >= 0;
        if (iid == __uuidof(IExpandCollapseProvider)) return n.expand_state() >= 0;
        if (iid == __uuidof(ISelectionItemProvider)) return n.selected_state() >= 0;
        if (iid == __uuidof(ISelectionProvider)) return n.role == Role::list;
        if (iid == __uuidof(IScrollProvider)) return n.scrolls;
        if (iid == __uuidof(IValueProvider)) return n.role == Role::document;
        return false;
    }
    static const IID* pattern_iid(PATTERNID p) {
        switch (p) {
        case UIA_InvokePatternId: return &__uuidof(IInvokeProvider);
        case UIA_TogglePatternId: return &__uuidof(IToggleProvider);
        case UIA_ExpandCollapsePatternId: return &__uuidof(IExpandCollapseProvider);
        case UIA_SelectionItemPatternId: return &__uuidof(ISelectionItemProvider);
        case UIA_SelectionPatternId: return &__uuidof(ISelectionProvider);
        case UIA_ScrollPatternId: return &__uuidof(IScrollProvider);
        case UIA_ValuePatternId: return &__uuidof(IValueProvider);
        default: return nullptr;
        }
    }
    void* pattern_ptr(REFIID iid) {
        if (iid == __uuidof(IInvokeProvider)) return static_cast<IInvokeProvider*>(this);
        if (iid == __uuidof(IToggleProvider)) return static_cast<IToggleProvider*>(this);
        if (iid == __uuidof(IExpandCollapseProvider)) return static_cast<IExpandCollapseProvider*>(this);
        if (iid == __uuidof(ISelectionItemProvider)) return static_cast<ISelectionItemProvider*>(this);
        if (iid == __uuidof(ISelectionProvider)) return static_cast<ISelectionProvider*>(this);
        if (iid == __uuidof(IScrollProvider)) return static_cast<IScrollProvider*>(this);
        if (iid == __uuidof(IValueProvider)) return static_cast<IValueProvider*>(this);
        return nullptr;
    }
    HRESULT set_expanded(bool on) {
        return on_ui([&]() -> HRESULT {
            Node* n = live_node();
            if (!n) return kNotAvailable;
            const int was = n->expand_state();
            n->set_expanded(on);
            if (was != (on ? 1 : 0) && ::UiaClientsAreListening()) {
                VARIANT o, v;
                ::VariantInit(&o);
                ::VariantInit(&v);
                o.vt = v.vt = VT_I4;
                o.lVal = was == 1 ? ExpandCollapseState_Expanded : ExpandCollapseState_Collapsed;
                v.lVal = on ? ExpandCollapseState_Expanded : ExpandCollapseState_Collapsed;
                ::UiaRaiseAutomationPropertyChangedEvent(static_cast<IRawElementProviderSimple*>(this),
                                                         UIA_ExpandCollapseExpandCollapseStatePropertyId, o, v);
            }
            return S_OK;
        });
    }

    LONG refs_ = 1;
    std::shared_ptr<Accessibility::State> s_;
    uint32_t id_;
};

}  // namespace

Accessibility::Accessibility(UiaSite* site) : s_(std::make_shared<State>()) { s_->site = site; }

Accessibility::~Accessibility() { detach(); }

LRESULT Accessibility::on_getobject(WPARAM wp, LPARAM lp) {
    if (!s_->site) return 0;
    s_->hwnd = s_->site->hwnd();
    auto* p = new Provider(s_, 0);
    const LRESULT r = ::UiaReturnRawElementProvider(s_->site->hwnd(), wp, lp, p);
    p->Release();
    return r;
}

void Accessibility::detach() {
    if (!s_->site) return;
    ::UiaReturnRawElementProvider(s_->site->hwnd(), 0, 0, nullptr);  // releases the providers UIA holds
    s_->site = nullptr;
}

void Accessibility::focus_changed(Node* n) {
    if (!s_->site || !n || !::UiaClientsAreListening()) return;
    Provider* p = Provider::of(s_, *n);
    ::UiaRaiseAutomationEvent(p, UIA_AutomationFocusChangedEventId);
    p->Release();
}

void Accessibility::notify(const std::u16string& text, const std::u16string& activity_id) {
    if (!s_->site) return;
    auto* p = new Provider(s_, 0);
    BSTR t = ::SysAllocStringLen(reinterpret_cast<const OLECHAR*>(text.data()), static_cast<UINT>(text.size()));
    BSTR a = ::SysAllocStringLen(reinterpret_cast<const OLECHAR*>(activity_id.data()), static_cast<UINT>(activity_id.size()));
    ::UiaRaiseNotificationEvent(p, NotificationKind_ItemAdded, NotificationProcessing_ImportantAll, t, a);
    ::SysFreeString(t);
    ::SysFreeString(a);
    p->Release();
}

void Accessibility::property_changed_name(Node& n) {
    if (!s_->site || !::UiaClientsAreListening()) return;
    Provider* p = Provider::of(s_, n);
    VARIANT o, v;
    ::VariantInit(&o);
    v.vt = VT_BSTR;
    v.bstrVal = ::SysAllocStringLen(reinterpret_cast<const OLECHAR*>(n.name.data()), static_cast<UINT>(n.name.size()));
    ::UiaRaiseAutomationPropertyChangedEvent(p, UIA_NamePropertyId, o, v);
    ::VariantClear(&v);
    p->Release();
}

}  // namespace wb::ui
