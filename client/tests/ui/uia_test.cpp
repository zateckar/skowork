// UI Automation tests of the native window (add-native-ui task 2.5): a real window on its own thread, driven by a
// UI Automation client like a screen reader would. Every control has a role (control type) and a name, the
// patterns act on the elements, every control is reachable with Tab, and announcements arrive as notification
// events.
#include <gtest/gtest.h>

#include <uiautomation.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <thread>

#include "ui/list.h"
#include "ui/richtext.h"
#include "ui/window.h"

using namespace wb;
using namespace wb::ui;

#ifndef UIA_ScrollPatternNoScroll
#define UIA_ScrollPatternNoScroll (-1.0)
#endif

namespace {

constexpr CLSID kCUIAutomation8 = {0xe22ad333, 0xb25f, 0x460c, {0x83, 0xd0, 0x05, 0x81, 0x10, 0x73, 0x95, 0xc9}};
const SystemColors kSys{0xFFFFFF, 0x000000, 0x0078D7, 0xFFFFFF, 0x0066CC, 0x6D6D6D, 0xF0F0F0, 0x000000};

// Runs a Window on its own thread with a message loop; run() executes code on that thread.
class UiThread {
public:
    explicit UiThread(std::function<void(Window&)> build) {
        thread_ = std::thread([this, build] {
            ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            WNDCLASSW wc{};
            wc.lpfnWndProc = [](HWND h, UINT m, WPARAM w, LPARAM l) -> LRESULT {
                if (m == WM_APP) {
                    (*reinterpret_cast<std::function<void()>*>(l))();
                    return 0;
                }
                return ::DefWindowProcW(h, m, w, l);
            };
            wc.hInstance = ::GetModuleHandleW(nullptr);
            wc.lpszClassName = L"WbUiTestRunner";
            ::RegisterClassW(&wc);
            runner_ = ::CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
            Window::Options o;
            o.title = L"Workbench UIA test";
            o.width = 900;
            o.height = 640;
            o.lang = "en";
            o.show = false;  // an in-process client must not receive events raised on the window thread (below)
            o.theme = make_theme(false, false, kSys);
            auto w = Window::create(o);
            if (w) {
                window_ = std::move(w).value();
                window_->confirm_link = [this](const std::u16string& url) {
                    opened_.push_back(url);
                    return false;  // never launch a browser from tests
                };
                build(*window_);
                window_->tree().layout_if_needed();
                hwnd_ = window_->hwnd();
            }
            {
                std::lock_guard<std::mutex> l(m_);
                ready_ = true;
            }
            cv_.notify_all();
            MSG msg;
            while (::GetMessageW(&msg, nullptr, 0, 0) > 0) {
                ::TranslateMessage(&msg);
                ::DispatchMessageW(&msg);
            }
            window_.reset();
            ::DestroyWindow(runner_);
            ::CoUninitialize();
        });
        std::unique_lock<std::mutex> l(m_);
        cv_.wait(l, [this] { return ready_; });
    }
    ~UiThread() {
        run([] { ::PostQuitMessage(0); });
        thread_.join();
    }
    HWND hwnd() const { return hwnd_; }
    Window& window() { return *window_; }
    void run(std::function<void()> f) { ::SendMessageW(runner_, WM_APP, 0, reinterpret_cast<LPARAM>(&f)); }
    std::vector<std::u16string> opened_;

private:
    std::thread thread_;
    std::mutex m_;
    std::condition_variable cv_;
    bool ready_ = false;
    HWND runner_ = nullptr;
    HWND hwnd_ = nullptr;
    std::unique_ptr<Window> window_;
};

std::wstring take(BSTR b) {
    std::wstring s = b ? std::wstring(b, ::SysStringLen(b)) : std::wstring();
    ::SysFreeString(b);
    return s;
}

struct El {
    com_ptr<IUIAutomationElement> el;
    std::wstring name, id, framework;
    CONTROLTYPEID type = 0;
    BOOL focusable = FALSE;
};

void walk(IUIAutomationTreeWalker* w, IUIAutomationElement* e, std::vector<El>& out, int depth = 0) {
    com_ptr<IUIAutomationElement> c;
    w->GetFirstChildElement(e, c.put());
    while (c) {
        El n;
        n.el = c;
        BSTR b = nullptr;
        c->get_CurrentName(&b);
        n.name = take(b);
        b = nullptr;
        c->get_CurrentAutomationId(&b);
        n.id = take(b);
        b = nullptr;
        c->get_CurrentFrameworkId(&b);
        n.framework = take(b);
        c->get_CurrentControlType(&n.type);
        c->get_CurrentIsKeyboardFocusable(&n.focusable);
        out.push_back(n);
        if (depth < 12) walk(w, c.get(), out, depth + 1);
        com_ptr<IUIAutomationElement> next;
        w->GetNextSiblingElement(c.get(), next.put());
        c = next;
    }
}

class Notifications final : public IUIAutomationNotificationEventHandler {
public:
    ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(::InterlockedIncrement(&refs_)); }
    ULONG STDMETHODCALLTYPE Release() override {
        const LONG n = ::InterlockedDecrement(&refs_);
        if (n == 0) delete this;
        return static_cast<ULONG>(n);
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IUIAutomationNotificationEventHandler)) {
            *out = static_cast<IUIAutomationNotificationEventHandler*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE HandleNotificationEvent(IUIAutomationElement*, enum NotificationKind, enum NotificationProcessing,
                                                      BSTR display, BSTR) override {
        std::lock_guard<std::mutex> l(m);
        texts.push_back(display ? display : L"");
        ::SetEvent(event);
        return S_OK;
    }
    HANDLE event = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::mutex m;
    std::vector<std::wstring> texts;

private:
    ~Notifications() { ::CloseHandle(event); }
    LONG refs_ = 1;
};

// The window the tests drive: the shell of a Thread view with every element kind.
struct Built {
    std::atomic<int> new_thread{0};
    std::atomic<bool> notify{false};
    std::atomic<size_t> selected{SIZE_MAX};
    std::atomic<DWORD> clicked_on{0}, selected_on{0};  // the threads the handlers ran on
};

void build_shell(Window& w, Built& b) {
    Ui& ui = w.ui();
    Node& c = w.content();
    c.axis = Axis::row;
    c.height = Sizing::fill;
    auto* nav = c.emplace<Box>("nav", Paint::surface);
    nav->width = Sizing::fixed;
    nav->fixed_w = 260;
    nav->height = Sizing::fill;
    nav->padding = Insets::all(8);
    nav->gap = 8;
    auto* add = nav->emplace<Button>(ui, "newThread", u"New thread", Button::Kind::primary, icon::add);
    add->on_click = [&b] {
        ++b.new_thread;
        b.clicked_on = ::GetCurrentThreadId();
    };
    auto* list = nav->emplace<ListView>("threads", [](size_t i, ListRow& row) {
        const std::string n = std::to_string(i);
        row.name = u"Thread " + std::u16string(n.begin(), n.end());
        row.measure = [](float wd) { return Size{wd, 40}; };
    }, 40.f);
    list->name = u"Threads";
    list->set_count(1000);
    list->on_select = [&b](size_t i) {
        b.selected = i;
        b.selected_on = ::GetCurrentThreadId();
    };
    c.emplace<Splitter>("split", u"Resize the thread list", *nav, 200, 420);
    auto* main = c.emplace<ScrollView>("feed");
    main->width = Sizing::fill;
    main->padding = Insets::all(16);
    main->gap = 12;
    auto* check = main->emplace<Checkbox>(ui, "notify", u"Notify me when done");
    check->on_change = [&b](bool on) { b.notify = on; };
    auto* card = main->emplace<Card>(ui, "tool", u"Tool call: web.fetch", true, false);
    card->body().emplace<Label>(ui, u"https://example.com", TextStyle::code);
    auto* md = main->emplace<MarkdownView>(ui, u"Answer from Workbench");
    md->set_markdown("Here is the **summary**. Details are in the [report](https://example.com/report).\n\n```\nok\n```\n");
    for (int i = 0; i < 30; ++i) main->emplace<Label>(ui, u"Line of the transcript to make the feed scroll.", TextStyle::body);
    main->emplace<IconButton>(ui, "more", icon::more, u"More options");
}

// A UI Automation client, created in the process's multithreaded apartment as a screen reader would use one.
com_ptr<IUIAutomation5> client() {
    com_ptr<IUIAutomation5> p;
    ::CoCreateInstance(kCUIAutomation8, nullptr, CLSCTX_INPROC_SERVER, __uuidof(IUIAutomation5), p.put_void());
    return p;
}

struct Uia {
    com_ptr<IUIAutomation5> uia;
    com_ptr<IUIAutomationElement> root;
    bool com_ = false;
    explicit Uia(HWND hwnd) {
        com_ = SUCCEEDED(::CoInitializeEx(nullptr, COINIT_MULTITHREADED));
        uia = client();
        if (uia) uia->ElementFromHandle(hwnd, root.put());
    }
    ~Uia() {
        if (uia) uia->RemoveAllEventHandlers();
        root.reset();
        uia.reset();
        if (com_) ::CoUninitialize();
    }
    std::vector<El> elements() {
        com_ptr<IUIAutomationTreeWalker> w;
        uia->get_ControlViewWalker(w.put());
        std::vector<El> out;
        walk(w.get(), root.get(), out);
        return out;
    }
    El find(const std::wstring& id) {
        for (auto& e : elements())
            if (e.id == id) return e;
        return {};
    }
};

template <class P>
com_ptr<P> pattern(const El& e, PATTERNID id) {
    com_ptr<P> p;
    if (e.el) e.el->GetCurrentPatternAs(id, __uuidof(P), p.put_void());
    return p;
}

}  // namespace

TEST(UiAutomation, EveryControlHasARoleAndAName) {
    Built b;
    UiThread t([&b](Window& w) { build_shell(w, b); });
    ASSERT_NE(t.hwnd(), nullptr);
    Uia u(t.hwnd());
    ASSERT_TRUE(u.root);
    const auto els = u.elements();
    ASSERT_GT(els.size(), 20u);
    std::map<std::wstring, CONTROLTYPEID> by_id;
    for (const El& e : els) {
        if (e.framework != L"Workbench") continue;  // the system's own parts of the window (its frame), not Workbench's elements
        by_id[e.id] = e.type;
        EXPECT_NE(e.type, 0) << std::string(e.id.begin(), e.id.end());
        const bool container = e.type == UIA_PaneControlTypeId || e.type == UIA_GroupControlTypeId;
        if (!container) EXPECT_FALSE(e.name.empty()) << "no name: " << std::string(e.id.begin(), e.id.end());
    }
    EXPECT_EQ(by_id[L"newThread"], UIA_ButtonControlTypeId);
    EXPECT_EQ(by_id[L"threads"], UIA_ListControlTypeId);
    EXPECT_EQ(by_id[L"row-0"], UIA_ListItemControlTypeId);
    EXPECT_EQ(by_id[L"split"], UIA_ThumbControlTypeId);
    EXPECT_EQ(by_id[L"notify"], UIA_CheckBoxControlTypeId);
    EXPECT_EQ(by_id[L"tool-header"], UIA_ButtonControlTypeId);
    EXPECT_EQ(by_id[L"link"], UIA_HyperlinkControlTypeId);
    EXPECT_EQ(by_id[L"more"], UIA_ButtonControlTypeId);
    // Virtualised: only the rows in view exist, not all 1000.
    size_t rows = 0;
    for (const El& e : els) rows += e.type == UIA_ListItemControlTypeId;
    EXPECT_GT(rows, 5u);
    EXPECT_LT(rows, 40u);
    // The same rule, checked on the element tree itself.
    std::vector<std::string> problems;
    t.run([&] { problems = a11y_problems(t.window().tree()); });
    EXPECT_TRUE(problems.empty()) << problems.front();
}

TEST(UiAutomation, PatternsActOnTheElements) {
    Built b;
    UiThread t([&b](Window& w) { build_shell(w, b); });
    Uia u(t.hwnd());
    ASSERT_TRUE(u.root);
    auto invoke = pattern<IUIAutomationInvokePattern>(u.find(L"newThread"), UIA_InvokePatternId);
    ASSERT_TRUE(invoke);
    EXPECT_TRUE(SUCCEEDED(invoke->Invoke()));
    EXPECT_EQ(b.new_thread.load(), 1);
    // The client calls from its own thread; the provider runs the call on the window's thread.
    DWORD window_thread = 0;
    t.run([&] { window_thread = ::GetCurrentThreadId(); });
    EXPECT_NE(window_thread, ::GetCurrentThreadId());
    EXPECT_EQ(b.clicked_on.load(), window_thread);

    auto toggle = pattern<IUIAutomationTogglePattern>(u.find(L"notify"), UIA_TogglePatternId);
    ASSERT_TRUE(toggle);
    toggle->Toggle();
    ToggleState ts = ToggleState_Off;
    toggle->get_CurrentToggleState(&ts);
    EXPECT_EQ(ts, ToggleState_On);
    EXPECT_TRUE(b.notify.load());

    auto expand = pattern<IUIAutomationExpandCollapsePattern>(u.find(L"tool-header"), UIA_ExpandCollapsePatternId);
    ASSERT_TRUE(expand);
    ExpandCollapseState es = ExpandCollapseState_LeafNode;
    expand->get_CurrentExpandCollapseState(&es);
    EXPECT_EQ(es, ExpandCollapseState_Collapsed);
    expand->Expand();
    expand->get_CurrentExpandCollapseState(&es);
    EXPECT_EQ(es, ExpandCollapseState_Expanded);

    auto select = pattern<IUIAutomationSelectionItemPattern>(u.find(L"row-3"), UIA_SelectionItemPatternId);
    ASSERT_TRUE(select);
    select->Select();
    EXPECT_EQ(b.selected.load(), 3u);
    EXPECT_EQ(b.selected_on.load(), window_thread);
    BOOL is = FALSE;
    select->get_CurrentIsSelected(&is);
    EXPECT_TRUE(is);
    auto selection = pattern<IUIAutomationSelectionPattern>(u.find(L"threads"), UIA_SelectionPatternId);
    ASSERT_TRUE(selection);
    com_ptr<IUIAutomationElementArray> sel;
    selection->GetCurrentSelection(sel.put());
    int n = 0;
    if (sel) sel->get_Length(&n);
    EXPECT_EQ(n, 1);

    auto scroll = pattern<IUIAutomationScrollPattern>(u.find(L"feed"), UIA_ScrollPatternId);
    ASSERT_TRUE(scroll);
    BOOL scrollable = FALSE;
    scroll->get_CurrentVerticallyScrollable(&scrollable);
    EXPECT_TRUE(scrollable);
    scroll->SetScrollPercent(UIA_ScrollPatternNoScroll, 100);
    double pct = 0;
    scroll->get_CurrentVerticalScrollPercent(&pct);
    EXPECT_NEAR(pct, 100.0, 0.5);
    scroll->SetScrollPercent(UIA_ScrollPatternNoScroll, 0);

    com_ptr<IUIAutomationValuePattern> value;
    for (const El& e : u.elements())
        if (e.type == UIA_DocumentControlTypeId) value = pattern<IUIAutomationValuePattern>(e, UIA_ValuePatternId);
    ASSERT_TRUE(value);
    BSTR v = nullptr;
    value->get_CurrentValue(&v);
    const std::wstring text = take(v);
    EXPECT_NE(text.find(L"Here is the summary."), std::wstring::npos);

    // A link asks before it opens (the test's confirm declines).
    auto link = pattern<IUIAutomationInvokePattern>(u.find(L"link"), UIA_InvokePatternId);
    ASSERT_TRUE(link);
    link->Invoke();
    t.run([] {});
    EXPECT_EQ(t.opened_, (std::vector<std::u16string>{u"https://example.com/report"}));
}

TEST(UiAutomation, EveryControlIsReachableWithTab) {
    Built b;
    UiThread t([&b](Window& w) { build_shell(w, b); });
    std::vector<std::string> order;
    std::vector<std::string> interactive;
    t.run([&] {
        Tree& tree = t.window().tree();
        tree.set_focus(nullptr);
        // Every control that acts (or is focusable) must be in the Tab cycle or inside a focusable container.
        std::function<void(Node&)> collect = [&](Node& n) {
            if (!n.visible) return;
            if (n.focusable) interactive.push_back(n.automation_id());
            for (auto& c : n.children()) collect(*c);
        };
        collect(tree.root());
        for (size_t i = 0; i < interactive.size() + 1; ++i) {
            ::SendMessageW(t.hwnd(), WM_KEYDOWN, VK_TAB, 0);
            order.push_back(tree.focused() ? tree.focused()->automation_id() : "");
        }
    });
    ASSERT_FALSE(interactive.empty());
    for (const std::string& id : interactive)
        EXPECT_NE(std::find(order.begin(), order.end(), id), order.end()) << id << " not reachable with Tab";
    EXPECT_EQ(order.front(), "newThread");
    EXPECT_EQ(order[1], "threads");
    EXPECT_EQ(order.back(), order.front());  // the cycle wraps
}

// Events: a screen reader listens from another process. (A client in the window's own process deadlocks when the
// window thread raises an event, so the listening client is this test executable started a second time.)
TEST(UiAutomationChild, ReceivesNotification) {
    const char* hwnd_text = std::getenv("WORKBENCH_UIA_HWND");
    if (!hwnd_text) GTEST_SKIP() << "runs as the child of UiAutomation.AnnouncementsArriveAsNotificationEvents";
    const HWND hwnd = reinterpret_cast<HWND>(static_cast<uintptr_t>(std::strtoull(hwnd_text, nullptr, 10)));
    Uia u(hwnd);
    ASSERT_TRUE(u.root);
    auto* handler = new Notifications();
    ASSERT_TRUE(SUCCEEDED(u.uia->AddNotificationEventHandler(u.root.get(), TreeScope_Subtree, nullptr, handler)));
    const std::string ready_name = std::getenv("WORKBENCH_UIA_READY");
    HANDLE ready = ::OpenEventA(EVENT_MODIFY_STATE, FALSE, ready_name.c_str());
    ASSERT_NE(ready, nullptr);
    ::SetEvent(ready);
    ::CloseHandle(ready);
    ASSERT_EQ(::WaitForSingleObject(handler->event, 10000), WAIT_OBJECT_0);
    std::wstring text;
    {
        std::lock_guard<std::mutex> l(handler->m);
        text = handler->texts.front();
    }
    std::ofstream(std::getenv("WORKBENCH_UIA_OUT"), std::ios::binary) << std::string(text.begin(), text.end());
    u.uia->RemoveNotificationEventHandler(u.root.get(), handler);
    handler->Release();
}

TEST(UiAutomation, AnnouncementsArriveAsNotificationEvents) {
    Built b;
    UiThread t([&b](Window& w) { build_shell(w, b); });
    const std::string ready_name = "WorkbenchUiaReady-" + std::to_string(::GetCurrentProcessId());
    HANDLE ready = ::CreateEventA(nullptr, TRUE, FALSE, ready_name.c_str());
    const auto out = std::filesystem::temp_directory_path() / ("workbench-uia-" + std::to_string(::GetCurrentProcessId()) + ".txt");
    std::filesystem::remove(out);
    ::SetEnvironmentVariableA("WORKBENCH_UIA_HWND", std::to_string(reinterpret_cast<uintptr_t>(t.hwnd())).c_str());
    ::SetEnvironmentVariableA("WORKBENCH_UIA_READY", ready_name.c_str());
    ::SetEnvironmentVariableW(L"WORKBENCH_UIA_OUT", out.wstring().c_str());
    wchar_t exe[MAX_PATH];
    ::GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring cmd = L"\"" + std::wstring(exe) + L"\" --gtest_filter=UiAutomationChild.ReceivesNotification";
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    ASSERT_TRUE(::CreateProcessW(exe, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi));
    ::SetEnvironmentVariableA("WORKBENCH_UIA_HWND", nullptr);
    const HANDLE waits[2] = {ready, pi.hProcess};
    const DWORD w = ::WaitForMultipleObjects(2, waits, FALSE, 20000);
    EXPECT_EQ(w, WAIT_OBJECT_0) << "the listening process did not start";
    if (w == WAIT_OBJECT_0)
        t.run([&] { t.window().announce(t.window().ui().strings.get("action.announce_approval", {{"tool", u"shell.run"}})); });
    if (::WaitForSingleObject(pi.hProcess, 20000) != WAIT_OBJECT_0) ::TerminateProcess(pi.hProcess, 1);
    DWORD code = 1;
    ::GetExitCodeProcess(pi.hProcess, &code);
    ::CloseHandle(pi.hProcess);
    ::CloseHandle(pi.hThread);
    ::CloseHandle(ready);
    EXPECT_EQ(code, 0u);
    std::ifstream in(out, std::ios::binary);
    const std::string got((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    std::filesystem::remove(out);
    EXPECT_EQ(got, "Approval required: shell.run");
}

TEST(UiWindow, PaintsAndFollowsResize) {
    Built b;
    UiThread t([&b](Window& w) { build_shell(w, b); });
    float before = 0, after = 0;
    t.run([&] {
        t.window().render();
        Node* feed = nullptr;
        for (auto& c : t.window().content().children())
            if (c->automation_id() == "feed") feed = c.get();
        before = feed->frame().w;
        ::SetWindowPos(t.hwnd(), nullptr, 0, 0, 1300, 800, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        t.window().render();
        after = feed->frame().w;
    });
    EXPECT_GT(after, before);
}

// client/desktop-host Window Title Bar (improve-window-frame D2): the caption band's parts as WM_NCHITTEST codes.
TEST(UiWindow, FrameHitTesting) {
    FrameLayout f;
    f.width = 1000;
    f.caption_height = 40;
    f.top_border = 8;
    f.minimize = {862, 0, 46, 40};
    f.maximize = {908, 0, 46, 40};
    f.close = {954, 0, 46, 40};
    EXPECT_EQ(frame_hit(f, 500, 3), HTTOP);  // the resize band
    EXPECT_EQ(frame_hit(f, 500, 20), HTCAPTION);  // a heading or a pane's background moves the window
    EXPECT_EQ(frame_hit(f, 880, 20), HTMINBUTTON);
    EXPECT_EQ(frame_hit(f, 930, 20), HTMAXBUTTON);  // what offers Snap layouts
    EXPECT_EQ(frame_hit(f, 990, 20), HTCLOSE);
    EXPECT_EQ(frame_hit(f, 500, 41), HTCLIENT);
    f.over_control = true;  // a button or field of the pane in the band
    EXPECT_EQ(frame_hit(f, 500, 20), HTCLIENT);
    EXPECT_EQ(frame_hit(f, 990, 20), HTCLOSE);
    f.over_control = false;
    f.maximized = true;  // no resizing while maximised
    EXPECT_EQ(frame_hit(f, 500, 3), HTCAPTION);
    EXPECT_EQ(frame_hit(f, 990, 3), HTCLOSE);
    f.caption_height = 0;  // the system title bar
    EXPECT_EQ(frame_hit(f, 990, 20), HTCLIENT);
}

// A header row's CaptionSpace takes the window buttons' width only while the row reaches the top-right corner.
TEST(UiWindow, CaptionSpaceKeepsTheCornerClear) {
    Tree tree;
    Node& r = tree.root();
    r.axis = Axis::row;
    r.height = Sizing::fill;
    auto* left = r.emplace<Node>("left");
    left->width = Sizing::fixed;
    left->fixed_w = 300;
    auto* left_head = left->emplace<Node>("left-head");
    left_head->axis = Axis::row;
    auto* left_space = left_head->emplace<CaptionSpace>();
    auto* right = r.emplace<Node>("right");
    auto* head = right->emplace<Node>("head");
    head->axis = Axis::row;
    auto* title = head->emplace<Node>("title");
    auto* space = head->emplace<CaptionSpace>();
    auto* rail = r.emplace<Node>("rail");
    rail->width = Sizing::fixed;
    rail->fixed_w = 200;
    rail->visible = false;
    tree.layout({1000, 600});
    EXPECT_EQ(space->frame().w, WindowButtons::kWidth);
    EXPECT_EQ(title->frame().right(), 1000 - WindowButtons::kWidth);
    EXPECT_EQ(left_space->frame().w, 0);
    rail->visible = true;  // another pane takes the corner
    tree.layout({1000, 600});
    EXPECT_EQ(space->frame().w, 0);
    EXPECT_EQ(title->frame().right(), 800);
}

// client/desktop-host Window Title Bar: no title bar row; the named window buttons at the top-right corner, and
// WM_NCHITTEST answered from the panes under the caption band.
TEST(UiWindow, WindowButtonsAndCaptionBand) {
    Built b;
    UiThread t([&b](Window& w) { build_shell(w, b); });
    ASSERT_NE(t.hwnd(), nullptr);
    Uia u(t.hwnd());
    ASSERT_TRUE(u.root);
    std::map<std::wstring, El> by_id;
    for (const El& e : u.elements()) by_id[e.id] = e;
    for (const auto& [id, name] : std::vector<std::pair<std::wstring, std::wstring>>{
             {L"title-bar-minimize", L"Minimize"}, {L"title-bar-maximize", L"Maximize"}, {L"title-bar-close", L"Close"}}) {
        ASSERT_TRUE(by_id.contains(id)) << std::string(id.begin(), id.end());
        EXPECT_EQ(by_id[id].name, name);
        EXPECT_EQ(by_id[id].type, UIA_ButtonControlTypeId);
    }
    Rect close, content, add;
    float width = 0;
    t.run([&] {
        WindowButtons* wb = t.window().window_buttons();
        ASSERT_NE(wb, nullptr);
        close = wb->button(CaptionButton::Action::close).frame();
        content = t.window().content().frame();
        width = t.window().renderer().size_dip().w;
        for (auto& c : t.window().content().children())
            if (c->automation_id() == "nav") add = c->children().front()->frame();
    });
    EXPECT_EQ(content.y, 0);  // no title bar row: the panes reach the top
    EXPECT_EQ(close.right(), width);
    EXPECT_EQ(close.y, 0);
    // Hit testing through the window: the close button, a pane's background in the band, a button in the band, the
    // content under the band.
    const auto hit = [&](float x, float y) {
        float k = 1;
        t.run([&] { k = t.window().scale(); });
        // In physical pixels, as the window (per-monitor DPI aware) reads them. This thread is not DPI aware, and
        // Windows would rescale both the conversion and the message's point for it.
        const DPI_AWARENESS_CONTEXT prev = ::SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        POINT p{static_cast<LONG>(x * k), static_cast<LONG>(y * k)};
        ::ClientToScreen(t.hwnd(), &p);
        const LRESULT r = ::SendMessageW(t.hwnd(), WM_NCHITTEST, 0, MAKELPARAM(p.x, p.y));
        if (prev) ::SetThreadDpiAwarenessContext(prev);
        return r;
    };
    EXPECT_EQ(hit(close.x + close.w / 2, close.y + close.h / 2), HTCLOSE);
    EXPECT_EQ(hit(4, 20), HTCAPTION);                                 // the navigation pane's padding
    EXPECT_EQ(hit(add.x + add.w / 2, add.y + add.h / 2), HTCLIENT);  // New thread
    EXPECT_EQ(hit(300, WindowButtons::kHeight + 20), HTCLIENT);
    std::vector<std::string> problems;
    t.run([&] { problems = a11y_problems(t.window().tree()); });
    EXPECT_TRUE(problems.empty()) << problems.front();
}
