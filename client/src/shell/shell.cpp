#include "shell/shell.h"

#include <shobjidl.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include "base/encoding.h"

#include "shell/derive.h"
#include "shell/shortcuts.h"
#include "ui/bundles.h"
#include "ui/text16.h"

namespace wb::shell {

using ui::Node;
using ui::Sizing;

Shell::Shell(HostLink& host, const MainWindow::Options& options, std::function<void()> on_closed)
    : host_(host), options_(options), on_closed_(std::move(on_closed)) {
    ui::Window::Options wo;
    wo.title = L"SkoWork";
    wo.width = options.width;
    wo.height = options.height;
    wo.lang = options.lang;
    wo.show = options.show;
    wo.theme = options.theme;
    auto w = ui::Window::create(wo);
    if (!w) return;
    window_ = std::move(w).value();
    language_ = window_->ui().strings.lang();
    repaint_timer_ = std::make_unique<RepaintTimer>(
        repaint_,
        [this](unsigned ms, std::function<void()> fn) {
            std::weak_ptr<int> alive = alive_;  // a repaint after the Shell is gone does nothing
            return window_->set_timer(ms, [alive, fn] {
                if (alive.lock()) fn();
            });
        },
        [this](uintptr_t id) { window_->cancel_timer(id); }, [] { return now_millis(); },
        [this] { thread_->update_feed(); });
    window_->on_key = [this](const ui::KeyEvent& e) { return on_key(e); };
    window_->on_closed = [this] {
        if (on_closed_) on_closed_();
    };
    window_->on_activate = [this](bool active) {
        if (!open_id().empty()) call("inbox.focus", {{"thread_id", open_id()}, {"focused", active}});
    };
    window_->tree().on_layout = [this](ui::Size viewport) { apply_layout(viewport); };
    build();
    load_status();
    call("settings.get", json::object(), [this](Result<json> r) {
        if (!r) return;
        if (const json* l = jobj(*r, "ui.language")) set_language(jstr(*l, "value", "system"));
        if (const json* d = jobj(*r, "ui.show_work_details")) set_show_work_details(jbool(*d, "value"));
    });
    load_folders();
}

void Shell::set_show_work_details(bool on) {
    if (show_work_details_ == on) return;
    show_work_details_ = on;
    if (thread_ && thread_->feed()) thread_->feed()->update();
}

Shell::~Shell() {
    alive_.reset();  // replies still on their way are dropped
    // A recording does not outlive its window (Dictation Privacy).
    if (dictation_.phase == DictationPhase::recording || dictation_.phase == DictationPhase::starting)
        host_.call("dictation.cancel", json::object(), [](Result<json>) {});
    if (window_) {
        window_->on_closed = nullptr;
        window_->tree().on_layout = nullptr;
    }
}

long long Shell::now() const { return options_.now ? options_.now() : now_millis(); }

const ThreadEntry* Shell::entry(const std::string& id) const {
    for (const auto& e : entries_)
        if (e.id == id) return &e;
    if (feed_ && feed_->entry.id == id) return &feed_->entry;
    return nullptr;
}

void Shell::call(const std::string& method, const json& params, Done done) {
    std::weak_ptr<int> alive = alive_;
    host_.call(method, params, [this, alive, done = std::move(done)](Result<json> r) {
        if (!alive.lock() || !done) return;
        // A feature that needs the control plane fails with a network error while offline; every view shows the same
        // words for it (add-offline-operation D6).
        if (!r && offline()) {
            static const std::set<std::string> kNetwork = {"connect", "timeout", "io", "tls", "unavailable", "offline",
                                                           "not_connected", "disconnected"};
            if (kNetwork.contains(r.error().code))
                return done(make_error(r.error().code, ui::to_utf8(t("common.offline_unavailable"))));
        }
        done(std::move(r));
    });
}

// ---- the element tree --------------------------------------------------------------------------------------------

namespace {

// Closes the drawer when the user presses outside it (light dismiss), and places it along the right edge.
class DrawerLayer : public Node {
public:
    explicit DrawerLayer(std::function<void()> dismiss) : Node("rail-drawer-layer"), dismiss_(std::move(dismiss)) {
        floating = true;
        visible = false;
    }
    bool arrange_children(ui::Tree& tree, const ui::Rect& in) override {
        for (const auto& c : children())
            if (c->visible) tree.place(*c, {in.right() - kRailWidth - 32, in.y, kRailWidth + 32, in.h});
        return true;
    }
    bool on_pointer(const ui::PointerEvent& e) override {
        if (e.kind != ui::PointerEvent::down) return false;
        for (const auto& c : children())
            if (c->visible && c->frame().contains(e.x, e.y)) return false;
        if (dismiss_) dismiss_();
        return true;
    }
    void draw(ui::Canvas& c) override { c.fill(frame(), ui::Paint::text, 0.08f); }

private:
    std::function<void()> dismiss_;
};

}  // namespace

void Shell::build() {
    Node& content = window_->content();
    content.axis = ui::Axis::column;
    content.height = Sizing::fill;
    auto* body = content.emplace<Node>("shell-body");
    body->axis = ui::Axis::row;
    body->height = Sizing::fill;
    nav_ = body->emplace<NavPane>(*this);
    auto* main = body->emplace<Node>("main");
    main->role = ui::Role::pane;
    main->name = t("nav.skip");
    main->height = Sizing::fill;
    banners_ = main->emplace<BannerArea>(*this);
    inbox_ = main->emplace<Node>("inbox-view");
    inbox_->axis = ui::Axis::row;
    inbox_->height = Sizing::fill;
    list_ = inbox_->emplace<ThreadListPane>(*this);
    splitter_ = inbox_->emplace<ui::Splitter>("list-splitter", t("list.label", {{"view", t("views.inbox")}}), *list_,
                                              kListMin, kListMax);
    thread_ = inbox_->emplace<ThreadPane>(*this);
    // Panes whose whole content scrolls start under the caption band, so nothing scrolls under the window buttons
    // (improve-window-frame D3); the band above them is empty and moves the window.
    auto* chat_area = inbox_->emplace<Node>("chat-area");
    chat_area->height = Sizing::fill;
    chat_area->padding.top = ui::WindowButtons::kHeight;
    chat_area->visible = false;
    chat_slot_ = chat_area->emplace<ui::ScrollView>("chat-slot");
    chat_slot_->role = ui::Role::pane;
    chat_slot_->name = t("titles.chats");
    chat_slot_->scrolls = true;
    chat_slot_->follow_end = true;  // a conversation stays at its newest message
    chat_slot_->width = Sizing::fill;
    chat_slot_->height = Sizing::fill;
    chat_slot_->padding = {16, 0, 16, 16};
    chat_slot_->visible = false;
    rail_ = inbox_->emplace<RailPane>(*this, "rail", false);
    auto* page_area = main->emplace<Node>("page-area");
    page_area->height = Sizing::fill;
    page_area->padding.top = ui::WindowButtons::kHeight;
    page_ = page_area->emplace<PagePane>(*this);
    drawer_layer_ = main->add(std::make_unique<DrawerLayer>([this] {
        drawer_open_ = false;
        window_->content().relayout();
    }));
    drawer_ = drawer_layer_->emplace<RailPane>(*this, "rail-drawer", true);
    status_bar_ = content.emplace<StatusBarView>(*this);
}

void Shell::apply_layout(ui::Size viewport) {
    const float w = viewport.w;
    nav_compact_ = w < kNavCompactBelow;
    nav_->set_compact(nav_compact_);
    single_pane_ = w < kSinglePaneBelow;
    rail_docked_ = w >= kRailDockedFrom;
    const bool inbox_route = route_.name == RouteName::inbox || route_.name == RouteName::thread ||
                             route_.name == RouteName::new_thread || route_.name == RouteName::chat;
    inbox_->visible = inbox_route;
    page_->visible = !inbox_route;
    page_->parent()->visible = page_->visible;
    const bool has_thread = route_.name == RouteName::thread || route_.name == RouteName::new_thread || route_.name == RouteName::chat;
    // A Chat takes the thread pane's place (add-chats-in-inbox D3).
    const bool chat_pane = route_.name == RouteName::chat;
    list_->visible = !single_pane_ || !has_thread;
    thread_->visible = !chat_pane && (!single_pane_ || has_thread);
    chat_slot_->visible = chat_pane && (!single_pane_ || has_thread);
    chat_slot_->parent()->visible = chat_slot_->visible;
    if (single_pane_) {
        list_->width = Sizing::fill;
    } else {
        list_->width = Sizing::fixed;
        if (list_->fixed_w <= 0) list_->fixed_w = kListDefault;
    }
    splitter_->visible = list_->visible && (thread_->visible || chat_slot_->visible);
    const bool rail_possible = thread_->visible && route_.name == RouteName::thread;
    rail_->visible = rail_possible && rail_docked_ && rail_open_;
    drawer_layer_->visible = rail_possible && !rail_docked_ && drawer_open_;
    drawer_->visible = drawer_layer_->visible;
}

// ---- routes --------------------------------------------------------------------------------------------------------

void Shell::navigate(const std::string& hash) {
    Route r = parse_route(hash).value_or(default_route());
    const Route before = route_;
    route_ = r;
    if (r.name == RouteName::inbox && r.view != view_) {
        view_ = r.view;
        reload_list();
    } else if (entries_.empty() && !list_loading_) {
        reload_list();
    }
    const std::string thread = r.name == RouteName::thread ? r.thread_id : std::string();
    if (thread != open_id_) {
        if (!open_id_.empty()) call("inbox.focus", {{"thread_id", open_id_}, {"focused", false}});
        open_id_ = thread;
        feed_.reset();
        feed_error_.clear();
        feed_in_flight_.clear();
        feed_again_ = false;
        live_ = {};
        context_ = nullptr;
        drawer_open_ = false;
        if (!thread.empty()) {
            call("threads.open", {{"thread_id", thread}});
            call("inbox.focus", {{"thread_id", thread}, {"focused", true}});
            load_feed();
            load_context();
        }
    }
    apply_route();
    // A search hit in the Thread that is already open: jump to its message now.
    if (r.msg && before.thread_id == thread && thread_->feed()) thread_->feed()->update();
}

void Shell::apply_route() {
    nav_->rebuild();
    list_->rebuild();
    if (const std::string shown = shown_entry(); !shown.empty()) list_->select_thread(shown);
    update_chat_slot();
    thread_->rebuild();
    rail_->rebuild();
    drawer_->rebuild();
    page_->rebuild();
    window_->content().relayout();
    window_->content().invalidate();
}

std::string Shell::shown_entry() const {
    if (route_.name == RouteName::chat) return "chat:" + route_.thread_id;
    return open_id_;
}

void Shell::update_chat_slot() {
    const bool conversation = route_.name == RouteName::chat;
    const std::string chat = conversation ? route_.thread_id : std::string();
    if (chats_ && conversation && chats_->open_chat() == chat) return;
    chat_slot_->clear_children();
    chats_ = nullptr;
    if (conversation) chats_ = static_cast<ChatsView*>(chat_slot_->add(std::make_unique<ChatsView>(*this, chat)));
    chat_slot_->relayout();
}

void Shell::bring_to_front() {
    HWND h = window_->hwnd();
    if (::IsIconic(h)) ::ShowWindow(h, SW_RESTORE);
    ::SetForegroundWindow(h);
}

// ---- data ------------------------------------------------------------------------------------------------------

void Shell::load_status() {
    call("app.status", json::object(), [this](Result<json> r) {
        if (r && is_app_status(*r)) {
            status_ = *r;
            refresh_status();
        }
    });
}

void Shell::load_folders(std::function<void()> then) {
    call("folders.list", json::object(), [this, then](Result<json> r) {
        if (!r) return;
        folders_.clear();
        if (const json* a = jobj(*r, "folders"); a && a->is_array())
            for (const json& f : *a) folders_.push_back({jstr(f, "id"), jstr(f, "name")});
        refresh_nav();
        if (then) then();
    });
}

void Shell::reload_list() {
    const uint64_t gen = ++list_generation_;
    list_loading_ = true;
    next_cursor_.clear();
    call("inbox.list", {{"view", view_}}, [this, gen](Result<json> r) {
        if (gen != list_generation_) return;
        list_loading_ = false;
        std::vector<ThreadEntry> list;
        if (r) {
            if (const json* a = jobj(*r, "entries"); a && a->is_array())
                for (const json& e : *a) list.push_back(entry_from(e));
            next_cursor_ = jstr(*r, "next_cursor");
            if (const json* c = jobj(*r, "counts")) counts_ = counts_from(*c);
        }
        set_entries(std::move(list));
        refresh_nav();
    });
}

void Shell::load_more() {
    if (list_loading_ || next_cursor_.empty()) return;
    const uint64_t gen = list_generation_;
    list_loading_ = true;
    call("inbox.list", {{"view", view_}, {"cursor", next_cursor_}}, [this, gen](Result<json> r) {
        if (gen != list_generation_) return;
        list_loading_ = false;
        if (!r) return;
        std::vector<ThreadEntry> list = entries_;
        if (const json* a = jobj(*r, "entries"); a && a->is_array())
            for (const json& e : *a) {
                ThreadEntry te = entry_from(e);
                if (std::none_of(list.begin(), list.end(), [&](const ThreadEntry& x) { return x.id == te.id; }))
                    list.push_back(std::move(te));
            }
        next_cursor_ = jstr(*r, "next_cursor");
        set_entries(std::move(list));
    });
}

void Shell::set_entries(std::vector<ThreadEntry> entries) {
    const std::string selected = list_->selected_thread();
    entries_ = std::move(entries);
    list_->rebuild();
    if (const std::string shown = shown_entry(); !shown.empty()) list_->select_thread(shown);
    else if (!selected.empty()) list_->select_thread(selected);
    window_->content().invalidate();
}

void Shell::load_feed() {
    const std::string id = open_id_;
    if (id.empty()) return;
    // One snapshot request at a time; events that ask for another while one is on its way get one more after it.
    if (feed_in_flight_ == id) {
        feed_again_ = true;
        return;
    }
    feed_in_flight_ = id;
    feed_again_ = false;
    const uint64_t gen = ++feed_generation_;
    const bool had_error = !feed_error_.empty();
    feed_error_.clear();
    if (had_error) thread_->rebuild();
    call("threads.feed", {{"thread_id", id}}, [this, id, gen](Result<json> r) {
        if (gen != feed_generation_ || id != open_id_) return;
        feed_in_flight_.clear();
        if (!r) {
            feed_.reset();
            feed_error_ = r.error().code == "not_found" ? "not_found" : r.error().message;
            refresh_thread();
            return;
        }
        const bool first = !feed_;
        feed_ = snapshot_from(*r);
        settle_live(*feed_, live_);
        if (first) refresh_thread();
        else {
            thread_->update_feed();
            rail_->rebuild();
            drawer_->rebuild();
        }
        if (feed_again_) load_feed();
    });
}

void Shell::fork(const std::string& thread_id, const std::string& run_id, long long seq) {
    call("threads.fork", {{"thread_id", thread_id}, {"run_id", run_id}, {"seq", seq}}, [this](Result<json> r) {
        if (!r) return alert(r.error());
        announce(t("thread.forked"));
        navigate(thread_hash(jstr(*r, "thread_id")));
    });
}

void Shell::retry(const std::string& thread_id) {
    call("threads.retry", {{"thread_id", thread_id}}, [this](Result<json> r) {
        if (!r) return alert(r.error());
        announce(t("errors.retried"));
    });
}

void Shell::compact(const std::string& thread_id) {
    call("threads.compact", {{"thread_id", thread_id}}, [this](Result<json> r) {
        if (!r) return alert(r.error());
        announce(count("status_bar.compacted", jint(*r, "messages_compacted")));
        load_feed();
        load_context();
    });
}

void Shell::load_context() {
    const std::string id = open_id_;
    if (id.empty()) return;
    call("threads.context", {{"thread_id", id}}, [this, id](Result<json> r) {
        if (id != open_id_) return;
        context_ = r ? *r : json(nullptr);
        status_bar_->rebuild();
        window_->content().relayout();
    });
}

std::u16string Shell::clock(long long unix_seconds) {
    if (unix_seconds <= 0) return u"—";
    const unsigned long long ticks = static_cast<unsigned long long>(unix_seconds) * 10000000ULL + 116444736000000000ULL;
    FILETIME ft{static_cast<DWORD>(ticks), static_cast<DWORD>(ticks >> 32)}, lft;
    SYSTEMTIME st;
    if (!::FileTimeToLocalFileTime(&ft, &lft) || !::FileTimeToSystemTime(&lft, &st)) return u"—";
    const std::wstring locale(language_.begin(), language_.end());
    wchar_t time[64]{};
    ::GetTimeFormatEx(locale.c_str(), TIME_NOSECONDS, &st, nullptr, time, 64);
    return std::u16string(reinterpret_cast<const char16_t*>(time));
}

std::u16string Shell::moment(long long unix_ms) {
    FILETIME ft;
    const unsigned long long ticks = static_cast<unsigned long long>(unix_ms) * 10000ULL + 116444736000000000ULL;
    ft.dwLowDateTime = static_cast<DWORD>(ticks);
    ft.dwHighDateTime = static_cast<DWORD>(ticks >> 32);
    FILETIME lft;
    SYSTEMTIME st;
    if (unix_ms <= 0 || !::FileTimeToLocalFileTime(&ft, &lft) || !::FileTimeToSystemTime(&lft, &st)) return {};
    const std::string& lang = ui().strings.lang();
    const std::wstring locale(lang.begin(), lang.end());  // "en", "de", "cs": locale names
    wchar_t date[64]{}, time[64]{};
    ::GetTimeFormatEx(locale.c_str(), TIME_NOSECONDS, &st, nullptr, time, 64);
    const std::u16string t16(reinterpret_cast<const char16_t*>(time));
    if (local_day_start(unix_ms, 0) == local_day_start(now(), 0)) return t16;
    ::GetDateFormatEx(locale.c_str(), DATE_SHORTDATE, &st, nullptr, date, 64, nullptr);
    return std::u16string(reinterpret_cast<const char16_t*>(date)) + u" " + t16;
}

bool Shell::copy_text(const std::string& utf8) {
    if (options_.copy_text) return options_.copy_text(utf8);
    const std::u16string text = ui::to_utf16(utf8);
    if (!::OpenClipboard(window_->hwnd())) return false;
    ::EmptyClipboard();
    const size_t bytes = (text.size() + 1) * sizeof(char16_t);
    HGLOBAL mem = ::GlobalAlloc(GMEM_MOVEABLE, bytes);
    bool ok = false;
    if (mem) {
        if (void* p = ::GlobalLock(mem)) {
            std::memcpy(p, text.c_str(), bytes);
            ::GlobalUnlock(mem);
            ok = ::SetClipboardData(CF_UNICODETEXT, mem) != nullptr;
        }
        if (!ok) ::GlobalFree(mem);
    }
    ::CloseClipboard();
    return ok;
}

void Shell::open_search() {
    // The search takes the Thread list's place, so it needs a view where the list shows.
    if ((route_.name != RouteName::inbox && route_.name != RouteName::thread) || (single_pane_ && !open_id_.empty()))
        navigate(view_hash(view_));
    list_->set_search(true);
    window_->tree().set_focus(&list_->search().field());
    window_->render();
}

void Shell::close_search() {
    if (!search_open()) return;
    list_->set_search(false);
    window_->tree().set_focus(&list_->list());
    window_->render();
}

bool Shell::search_open() const { return list_->search_open(); }

void Shell::show_shortcuts() {
    if (window_->modal().top()) return;
    auto d = std::make_unique<ui::Dialog>(ui(), "shortcuts-dialog", t("shortcuts.title"), 480);
    auto* list = d->body().emplace<ui::Node>("shortcuts-list");
    list->role = ui::Role::list;
    list->name = t("shortcuts.title");
    list->gap = 2;
    size_t i = 0;
    for (const CatalogueEntry& e : catalogue()) {
        auto* row = list->emplace<ui::Node>("shortcut-" + std::to_string(i++));
        row->axis = ui::Axis::row;
        row->gap = 12;
        row->padding = ui::Insets::xy(0, 4);
        row->role = ui::Role::list_item;
        const std::u16string keys = ui::to_utf16(e.keys), action = t(e.label_key);
        row->name = keys + u": " + action;
        auto* k = row->emplace<ui::Label>(ui(), keys, ui::TextStyle::code, ui::Paint::text, false);
        k->role = ui::Role::none;
        k->width = ui::Sizing::fixed;
        k->fixed_w = 150;
        row->emplace<ui::Label>(ui(), action, ui::TextStyle::body, ui::Paint::text, true)->role = ui::Role::none;
    }
    ui::Dialog* dp = d.get();
    ui::Button* close = d->add_button("shortcuts-close", t("common.close"), ui::Button::Kind::primary, [dp] { dp->close(); });
    window_->modal().show(std::move(d));
    window_->tree().set_focus(close);
    window_->render();
}

void Shell::sign_in() {
    call("identity.sign_in", json::object(), [this](Result<json> r) {
        if (!r) alert(r.error());
    });
}

std::string Shell::runs_blocked() const {
    const json* agent = jobj(status_, "agent");
    if (!agent) return {};
    if (!jbool(*agent, "available", true)) return "unavailable";
    return jstr(*agent, "start_blocked");
}

bool Shell::offline() const {
    const json* agent = jobj(status_, "agent");
    return agent && jbool(*agent, "offline");
}

std::vector<std::wstring> Shell::pick_files() {
    if (options_.pick_files) return options_.pick_files();
    std::vector<std::wstring> out;
    com_ptr<IFileOpenDialog> d;
    if (FAILED(::CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(d.put())))) return out;
    FILEOPENDIALOGOPTIONS o = 0;
    d->GetOptions(&o);
    d->SetOptions(o | FOS_ALLOWMULTISELECT | FOS_FILEMUSTEXIST | FOS_FORCEFILESYSTEM);
    const std::u16string label = t("composer.attach");
    const std::wstring title(label.begin(), label.end());
    d->SetTitle(title.c_str());
    if (FAILED(d->Show(window_->hwnd()))) return out;  // cancelled
    com_ptr<IShellItemArray> items;
    if (FAILED(d->GetResults(items.put()))) return out;
    DWORD n = 0;
    items->GetCount(&n);
    for (DWORD i = 0; i < n; ++i) {
        com_ptr<IShellItem> item;
        PWSTR path = nullptr;
        if (SUCCEEDED(items->GetItemAt(i, item.put())) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
            out.emplace_back(path);
            ::CoTaskMemFree(path);
        }
    }
    return out;
}

std::wstring Shell::pick_folder() {
    if (options_.pick_folder) return options_.pick_folder();
    com_ptr<IFileOpenDialog> d;
    if (FAILED(::CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(d.put())))) return {};
    FILEOPENDIALOGOPTIONS o = 0;
    d->GetOptions(&o);
    d->SetOptions(o | FOS_PICKFOLDERS | FOS_PATHMUSTEXIST | FOS_FORCEFILESYSTEM);
    const std::u16string label = t("settings.files.pick");
    const std::wstring title(label.begin(), label.end());
    d->SetTitle(title.c_str());
    if (FAILED(d->Show(window_->hwnd()))) return {};  // cancelled
    com_ptr<IShellItem> item;
    PWSTR path = nullptr;
    std::wstring out;
    if (SUCCEEDED(d->GetResult(item.put())) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
        out = path;
        ::CoTaskMemFree(path);
    }
    return out;
}

// ---- images in the feed (add-image-generation) ----------------------------------------------------------------------

void Shell::load_image(const std::string& thread_id, const FileRef& file, std::function<void()> done) {
    auto& slot = images_[file.id];
    if (slot && slot->done) {
        if (done) done();
        return;
    }
    const bool first = !slot;
    if (first) slot = std::make_shared<LoadedImage>();
    if (done) slot->waiting.push_back(std::move(done));
    if (!first) return;  // already loading
    std::shared_ptr<LoadedImage> img = slot;
    std::weak_ptr<int> alive = alive_;
    auto finish = [this, alive, img] {
        if (!alive.lock()) return;
        img->done = true;
        auto waiting = std::move(img->waiting);
        img->waiting.clear();
        for (auto& w : waiting) w();
    };
    call("threads.attachment", {{"thread_id", thread_id}, {"id", file.id}}, [this, alive, img, finish](Result<json> r) {
        if (!alive.lock()) return;
        if (!r) {
            img->error = r.error().message;
            return finish();
        }
        auto bytes = base64_decode(jstr(*r, "data"));
        if (!bytes || bytes->empty()) {
            img->error = "the image data could not be read";
            return finish();
        }
        img->bytes.assign(bytes->begin(), bytes->end());
        // Decoding a large image takes tens of milliseconds: off the window's thread when the app gives a worker.
        auto decode = [img] {
            const HRESULT co = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            auto bmp = ui::decode_image(img->bytes.data(), img->bytes.size(), 2048);
            if (bmp) img->bitmap = std::move(*bmp);
            else img->error = bmp.error().message;
            if (SUCCEEDED(co)) ::CoUninitialize();
        };
        if (options_.background && options_.post) {
            options_.background([decode, finish, post = options_.post] {
                decode();
                post(finish);
            });
        } else {
            decode();
            finish();
        }
    });
}

const Shell::LoadedImage* Shell::image(const std::string& id) const {
    const auto it = images_.find(id);
    return it != images_.end() && it->second->done ? it->second.get() : nullptr;
}

void Shell::show_image(const FileRef& file) {
    const LoadedImage* img = image(file.id);
    if (!img || !img->bitmap || window_->modal().top()) return;
    const ui::Rect area = window_->content().frame();
    const float w = std::max(320.f, std::min(area.w - 80, static_cast<float>(file.width) + 48));
    auto d = std::make_unique<ui::Dialog>(ui(), "image-dialog", ui::to_utf16(file.name), w);
    d->body().emplace<ui::ImageView>(
        t("feed.image.label", {{"width", ui::to_utf16(std::to_string(file.width))}, {"height", ui::to_utf16(std::to_string(file.height))}}),
        img->bitmap, std::max(200.f, area.h - 200), "image-dialog-image");
    ui::Dialog* dp = d.get();
    const FileRef f = file;
    d->add_button("image-dialog-save", t("feed.image.save"), ui::Button::Kind::standard, [this, f] { save_image(f); });
    ui::Button* close = d->add_button("image-dialog-close", t("common.close"), ui::Button::Kind::primary, [dp] { dp->close(); });
    window_->modal().show(std::move(d));
    window_->tree().set_focus(close);
    window_->render();
}

void Shell::save_image(const FileRef& file) {
    const LoadedImage* img = image(file.id);
    if (!img || img->bytes.empty()) return;
    const std::u16string name16 = ui::to_utf16(file.name.empty() ? std::string("image.png") : file.name);
    const std::wstring suggested(name16.begin(), name16.end());
    std::wstring path;
    if (options_.save_file) {
        path = options_.save_file(suggested);
    } else {
        com_ptr<IFileSaveDialog> d;
        if (FAILED(::CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(d.put())))) return;
        FILEOPENDIALOGOPTIONS o = 0;
        d->GetOptions(&o);
        d->SetOptions(o | FOS_OVERWRITEPROMPT | FOS_FORCEFILESYSTEM);
        d->SetFileName(suggested.c_str());
        const auto dot = suggested.rfind(L'.');
        if (dot != std::wstring::npos) d->SetDefaultExtension(suggested.c_str() + dot + 1);
        if (FAILED(d->Show(window_->hwnd()))) return;  // cancelled
        com_ptr<IShellItem> item;
        PWSTR p = nullptr;
        if (FAILED(d->GetResult(item.put())) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &p))) return;
        path = p;
        ::CoTaskMemFree(p);
    }
    if (path.empty()) return;
    std::ofstream out(std::filesystem::path(path), std::ios::binary | std::ios::trunc);
    out.write(img->bytes.data(), static_cast<std::streamsize>(img->bytes.size()));
    out.close();
    if (!out) {
        announce(t("feed.image.save_failed"));
        return;
    }
    announce(t("feed.image.saved", {{"path", std::u16string(path.begin(), path.end())}}));
}

void Shell::open_thread(const std::string& id) {
    // A Chat entry opens its conversation in the reading pane (add-chats-in-inbox D3).
    if (id.starts_with("chat:")) navigate("#/chats/" + id.substr(5));
    else navigate(thread_hash(id));
}

// ---- host events ---------------------------------------------------------------------------------------------------

void Shell::emit(const std::string& event, const json& data) {
    if (event == "status") {
        if (is_app_status(data)) {
            const bool nav = status_.value("messaging", json()) != data.value("messaging", json());
            const bool chats_on = jbool(status_.value("messaging", json::object()), "enabled");
            status_ = data;
            refresh_status();
            if (nav) refresh_nav();
            // Messaging switched on or off: Chat entries come or go (add-chats-in-inbox D1).
            if (chats_on != jbool(status_.value("messaging", json::object()), "enabled")) reload_list();
        }
    } else if (event == "navigate") {
        const std::string r = jstr(data, "route");
        if (!r.empty()) navigate(r);
    } else if (event == "inbox.changed") {
        on_inbox_changed(data);
    } else if (event == "run.event") {
        on_run_event(data);
    } else if (event == "automations.changed") {
        if (is_page(route_.name)) page_->automations_changed();
    } else if (event == "mail.changed") {
        if (is_page(route_.name)) page_->mail_changed();
    } else if (event == "knowledge.changed") {
        if (is_page(route_.name)) page_->knowledge_changed();
    } else if (event == "meetings.changed") {
        if (is_page(route_.name)) page_->meetings_changed();
    } else if (event == "apps.changed") {
        if (is_page(route_.name)) page_->apps_changed();
    } else if (event == "messaging.changed") {
        // client/user-messaging: the unread count of the navigation entry, and the Chats view.
        if (status_.is_object() && status_.contains("messaging") && status_["messaging"].is_object() && data.contains("unread")) {
            if (status_["messaging"].value("unread", json()) != data["unread"]) {
                status_["messaging"]["unread"] = data["unread"];
                refresh_nav();
            }
        }
        if (chats_) chats_->changed(data);
    } else if (event == "speech.changed") {
        if (route_.name == RouteName::settings && page_) page_->speech_changed(data);
    } else if (event == "dictation.changed") {
        // The 5-minute limit stopped the recording: transcribe it as Stop would (add-dictation D1).
        if (jstr(data, "state") == "limit") stop_dictation();
        // Policy turned dictation off: the host discarded the recording.
        if (jstr(data, "state") == "discarded") discard_dictation();
    }
}

void Shell::on_inbox_changed(const json& data) {
    if (const json* c = jobj(data, "counts")) counts_ = counts_from(*c);
    const std::string id = jstr(data, "thread_id");
    if (id.empty()) {  // folders changed
        load_folders();
        reload_list();
        return;
    }
    if (jbool(data, "deleted")) {
        std::vector<ThreadEntry> list = entries_;
        std::erase_if(list, [&](const ThreadEntry& e) { return e.id == id; });
        set_entries(std::move(list));
        if (id == shown_entry()) navigate(view_hash(view_));
    } else if (const json* e = jobj(data, "entry")) {
        const ThreadEntry te = entry_from(*e);
        std::vector<ThreadEntry> list = entries_;
        apply_entry(list, te, view_, !next_cursor_.empty());
        set_entries(std::move(list));
        if (feed_ && feed_->entry.id == id) {
            feed_->entry = te;
            thread_->rebuild();
        }
    }
    refresh_nav();
}

void Shell::feed_changed(bool streamed) {
    // A stored change repaints at once and covers a repaint that was waiting; a streamed one is limited (RepaintTimer).
    if (!repaint_timer_) return;  // no window
    if (streamed) {
        repaint_timer_->streamed_change();
    } else {
        repaint_timer_->stored_change();
    }
}

void Shell::on_run_event(const json& data) {
    if (!feed_) return;
    const LiveRunEvent ev = live_event_from(data);
    if (ev.thread_id != feed_->entry.id) return;
    if (ev.kind == "action.created") {  // read out like an alert region, also for standalone Action items
        const json* p = jobj(ev.data, "payload");
        const json payload = p ? *p : json::object();
        announce(jstr(ev.data, "kind") == "approval" ? t("approval.announce", {{"tool", ui::to_utf16(jstr(payload, "tool"))}})
                                                     : t("question.announce", {{"prompt", ui::to_utf16(jstr(payload, "prompt"))}}));
    }
    if (apply_live_event(*feed_, live_, ev) == Apply::refetch) load_feed();
    feed_changed(ev.kind == "text.delta" || ev.kind == "reasoning.delta");
    if (ev.kind == "subagent" || ev.kind == "run.state") {
        rail_->rebuild();
        drawer_->rebuild();
    }
    // Context usage changes when a Run ends or the Thread is compacted.
    if (ev.kind == "compaction" ||
        (ev.kind == "run.state" && (jstr(ev.data, "state") == "succeeded" || jstr(ev.data, "state") == "failed" ||
                                    jstr(ev.data, "state") == "interrupted")))
        load_context();
}

void Shell::set_language(const std::string& lang) {
    const std::string resolved = ui::resolve_language(lang, ui::system_language());
    if (resolved == language_) return;
    language_ = resolved;
    window_->ui().strings = ui::load_bundle(resolved);
    window_->ui().texts().clear();
    refresh_status();
    apply_route();
}

// ---- refreshes -----------------------------------------------------------------------------------------------------

void Shell::refresh_nav() {
    nav_->rebuild();
    window_->content().relayout();
}

void Shell::refresh_list() {
    list_->rebuild();
    if (const std::string shown = shown_entry(); !shown.empty()) list_->select_thread(shown);
}

void Shell::refresh_thread() {
    thread_->rebuild();
    rail_->rebuild();
    drawer_->rebuild();
    window_->content().relayout();
}

void Shell::refresh_status() {
    banners_->rebuild();
    status_bar_->rebuild();
    thread_->update_composer();
    dictation_changed();
    if (is_page(route_.name)) page_->rebuild();
    window_->content().relayout();
}

// ---- actions ---------------------------------------------------------------------------------------------------

void Shell::toggle_rail() {
    if (rail_docked_) rail_open_ = !rail_open_;
    else drawer_open_ = !drawer_open_;
    thread_->rebuild();
    window_->content().relayout();
}

void Shell::archive(const std::string& id, bool archived) {
    call("threads.archive", {{"thread_id", id}, {"archived", archived}}, [this, archived](Result<json> r) {
        if (r) announce(t(archived ? "thread.archived" : "thread.restored"));
    });
}

void Shell::mark_unread(const std::string& id) {
    call("threads.mark_unread", {{"thread_id", id}, {"unread", true}}, [this](Result<json> r) {
        if (r) announce(t("thread.marked_unread"));
    });
    if (id == open_id_) navigate(view_hash(view_));  // reading it again would mark it read
}

void Shell::pin(const std::string& id, bool pinned) {
    call("threads.pin", {{"thread_id", id}, {"pinned", pinned}}, [this, pinned](Result<json> r) {
        if (r) announce(t(pinned ? "thread.pinned" : "thread.unpinned"));
    });
}

void Shell::move_to(const std::string& id, const std::string& folder_id) {
    std::u16string name = t("views.inbox");
    for (const auto& f : folders_)
        if (f.id == folder_id) name = ui::to_utf16(f.name);
    call("threads.move", {{"thread_id", id}, {"folder_id", folder_id}}, [this, name](Result<json> r) {
        if (r) announce(t("thread.moved", {{"folder", name}}));
    });
}

void Shell::ask_delete(const std::string& id) {
    auto d = std::make_unique<ui::Dialog>(ui(), "delete-dialog", t("thread.delete_title"));
    d->body().emplace<ui::Label>(ui(), t("thread.delete_body"));
    ui::Dialog* dp = d.get();
    d->add_button("delete-confirm", t("thread.delete_confirm"), ui::Button::Kind::primary, [this, dp, id] {
        dp->close();
        call("threads.delete", {{"thread_id", id}, {"confirm", true}}, [this](Result<json> r) {
            if (r) announce(t("thread.deleted"));
        });
    });
    ui::Button* cancel = d->add_button("delete-cancel", t("thread.cancel"), ui::Button::Kind::standard, [dp] { dp->close(); });
    window_->modal().show(std::move(d));
    window_->tree().set_focus(cancel);  // a destructive action is never the default
    window_->render();
}

namespace {

// A dialog with one text field and Save / Cancel.
void ask_text(Shell& s, const std::string& id, const std::u16string& title, const std::u16string& label,
              const std::u16string& initial, std::function<void(const std::string&)> save) {
    auto d = std::make_unique<ui::Dialog>(s.ui(), id, title);
    auto* field = d->body().emplace<ui::TextField>(s.ui(), id + "-field", label);
    field->set_text(initial);
    ui::Dialog* dp = d.get();
    auto commit = [dp, field, save] {
        std::string text = ui::to_utf8(field->text());
        const auto b = text.find_first_not_of(" \t");
        text = b == std::string::npos ? std::string() : text.substr(b, text.find_last_not_of(" \t") - b + 1);
        if (text.empty()) return;
        dp->close();
        save(text);
    };
    field->on_key_down = [commit](const ui::KeyEvent& e) {
        if (e.key != ui::kKeyEnter || e.shift || e.ctrl || e.alt) return false;
        commit();
        return true;
    };
    d->add_button(id + "-save", s.t("thread.save"), ui::Button::Kind::primary, commit);
    d->add_button(id + "-cancel", s.t("thread.cancel"), ui::Button::Kind::standard, [dp] { dp->close(); });
    s.window().modal().show(std::move(d));
    s.window().tree().set_focus(field);
    field->select_all();
    s.window().render();
}

}  // namespace

void Shell::ask_text_value(const std::string& id, const std::u16string& title, const std::u16string& label,
                           const std::u16string& initial, std::function<void(const std::string&)> save) {
    ask_text(*this, id, title, label, initial, std::move(save));
}

void Shell::ask_rename(const std::string& id) {
    const ThreadEntry* e = entry(id);
    ask_text(*this, "rename-dialog", t("thread.rename"), t("thread.title_label"), e ? ui::to_utf16(e->title) : u"",
             [this, id](const std::string& title) {
                 call("threads.rename", {{"thread_id", id}, {"title", title}}, [this](Result<json> r) {
                     if (r) announce(t("thread.renamed"));
                 });
             });
}

void Shell::ask_new_folder() {
    ask_text(*this, "folder-dialog", t("nav.new_folder"), t("nav.new_folder"), u"", [this](const std::string& name) {
        call("folders.create", {{"name", name}}, [this](Result<json> r) {
            if (r) load_folders();
        });
    });
}

void Shell::show_folder_menu(const std::string& folder_id, const ui::Rect& anchor) {
    auto menu = std::make_unique<ui::Menu>("folder-menu", t("folder.options"));
    menu->add_item(ui(), "folder-menu-rename", t("folder.rename"), [this, folder_id] { ask_rename_folder(folder_id); });
    menu->add_item(ui(), "folder-menu-delete", t("folder.delete"), [this, folder_id] { ask_delete_folder(folder_id); },
                   ui::icon::del);
    window_->overlay().show(std::move(menu), anchor, true);
    window_->render();
}

void Shell::ask_rename_folder(const std::string& folder_id) {
    std::u16string name;
    for (const auto& f : folders_)
        if (f.id == folder_id) name = ui::to_utf16(f.name);
    ask_text(*this, "folder-rename-dialog", t("folder.rename"), t("folder.name_label"), name, [this, folder_id](const std::string& n) {
        call("folders.rename", {{"id", folder_id}, {"name", n}}, [this](Result<json> r) {
            if (!r) return alert(r.error());
            announce(t("folder.renamed"));
            load_folders([this] { refresh_list(); });  // the list's title is the folder's name
        });
    });
}

// Deleting a folder moves its Threads to the Inbox (client/work-inbox Folders); the Inbox is shown afterwards.
void Shell::ask_delete_folder(const std::string& folder_id) {
    std::u16string name;
    for (const auto& f : folders_)
        if (f.id == folder_id) name = ui::to_utf16(f.name);
    auto d = std::make_unique<ui::Dialog>(ui(), "folder-delete-dialog", t("folder.delete_title", {{"folder", name}}));
    d->body().emplace<ui::Label>(ui(), t("folder.delete_body"));
    ui::Dialog* dp = d.get();
    d->add_button("folder-delete-confirm", t("folder.delete"), ui::Button::Kind::primary, [this, dp, folder_id] {
        dp->close();
        call("folders.delete", {{"id", folder_id}}, [this, folder_id](Result<json> r) {
            if (!r) return alert(r.error());
            announce(t("folder.deleted"));
            if (view_ == "folder:" + folder_id) navigate("#/inbox");
            load_folders();
        });
    });
    ui::Button* cancel = d->add_button("folder-delete-cancel", t("thread.cancel"), ui::Button::Kind::standard, [dp] { dp->close(); });
    window_->modal().show(std::move(d));
    window_->tree().set_focus(cancel);  // a destructive action is never the default
    window_->render();
}

void Shell::show_thread_menu(const std::string& id, const ui::Rect& anchor) {
    const ThreadEntry* e = entry(id);
    if (!e) return;
    const ThreadEntry te = *e;
    auto menu = std::make_unique<ui::Menu>("thread-menu", t("thread.toolbar"));
    // A Chat entry: Pin, Move to folder, Archive and Leave Chat; it is read when opened and never deleted (D3).
    if (te.is_chat()) {
        menu->add_item(ui(), "menu-archive", t(te.archived ? "thread.restore" : "thread.archive"),
                       [this, id, a = te.archived] { archive(id, !a); }, ui::icon::archive);
        menu->add_item(ui(), "menu-pin", t(te.pinned ? "thread.unpin" : "thread.pin"), [this, id, p = te.pinned] { pin(id, !p); },
                       ui::icon::pin);
        if (!te.folder_id.empty())
            menu->add_item(ui(), "menu-move-inbox", t("thread.move_to") + u": " + t("thread.no_folder"),
                           [this, id] { move_to(id, ""); }, ui::icon::folder);
        for (const auto& f : folders_)
            if (f.id != te.folder_id)
                menu->add_item(ui(), "menu-move-" + f.id, t("thread.move_to") + u": " + ui::to_utf16(f.name),
                               [this, id, fid = f.id] { move_to(id, fid); }, ui::icon::folder);
        menu->add_item(ui(), "menu-leave", t("list.leave_chat"), [this, id] { ask_leave_chat(id); });
        window_->overlay().show(std::move(menu), anchor, true);
        window_->render();
        return;
    }
    menu->add_item(ui(), "menu-archive", t(te.archived ? "thread.restore" : "thread.archive"),
                   [this, id, a = te.archived] { archive(id, !a); }, ui::icon::archive);
    menu->add_item(ui(), "menu-unread", t("thread.mark_unread"), [this, id] { mark_unread(id); });
    menu->add_item(ui(), "menu-pin", t(te.pinned ? "thread.unpin" : "thread.pin"), [this, id, p = te.pinned] { pin(id, !p); },
                   ui::icon::pin);
    menu->add_item(ui(), "menu-rename", t("thread.rename"), [this, id] { ask_rename(id); });
    // Save Thread As Skill: the editor opens with a draft from this Thread.
    menu->add_item(ui(), "menu-save-skill", t("thread.save_as_skill"), [this, id] { navigate("#/skills/new?thread=" + id); });
    if (!te.folder_id.empty())
        menu->add_item(ui(), "menu-move-inbox", t("thread.move_to") + u": " + t("thread.no_folder"),
                       [this, id] { move_to(id, ""); }, ui::icon::folder);
    for (const auto& f : folders_)
        if (f.id != te.folder_id)
            menu->add_item(ui(), "menu-move-" + f.id, t("thread.move_to") + u": " + ui::to_utf16(f.name),
                           [this, id, fid = f.id] { move_to(id, fid); }, ui::icon::folder);
    menu->add_item(ui(), "menu-delete", t("thread.delete"), [this, id] { ask_delete(id); }, ui::icon::del);
    window_->overlay().show(std::move(menu), anchor, true);
    window_->render();
}

void Shell::ask_leave_chat(const std::string& entry_id) {
    const ThreadEntry* e = entry(entry_id);
    if (!e || !e->is_chat()) return;
    const std::string chat = e->chat_id();
    const std::u16string title = ui::to_utf16(e->title);
    auto d = std::make_unique<ui::Dialog>(ui(), "chat-leave-dialog", t("chats.leave_title"));
    d->body().emplace<ui::Label>(ui(), t("chats.leave_body", {{"name", title}}), ui::TextStyle::body, ui::Paint::text, true,
                                 "chat-leave-dialog-body");
    ui::Dialog* dp = d.get();
    d->add_button("chat-leave-dialog-confirm", t("chats.leave"), ui::Button::Kind::primary, [this, dp, chat, title] {
        dp->close();
        call("messaging.leave", {{"chat", chat}}, [this, title](Result<json> r) {
            if (!r) return alert(r.error());
            announce(t("chats.left", {{"name", title}}));
        });
    });
    ui::Button* cancel = d->add_button("chat-leave-dialog-cancel", t("chats.cancel"), ui::Button::Kind::standard, [dp] { dp->close(); });
    window_->modal().show(std::move(d));
    window_->tree().set_focus(cancel);
    window_->render();
}

// ---- keyboard ----------------------------------------------------------------------------------------------------

bool Shell::on_key(const ui::KeyEvent& e) {
    Node* f = window_->tree().focused();
    if (e.key == ui::kKeyEscape && !e.ctrl && !e.alt && drawer_open_ && drawer_layer_->visible) {
        toggle_rail();  // Escape closes the Activity drawer
        return true;
    }
    KeyContext ctx;
    for (ui::TextField* tf : ui().fields)
        if (tf->has_window_focus()) ctx.typing = true;
    const bool in_list = f == &list_->list();
    ctx.on_list_entry = in_list && !list_->selected_thread().empty();
    ctx.thread_open = !open_id_.empty();
    const auto cmd = command_for(e, ctx);
    if (!cmd) return false;
    const std::string target = ctx.on_list_entry ? list_->selected_thread() : open_id_;
    const bool chat = target.starts_with("chat:");  // read when opened, never deleted (add-chats-in-inbox D3)
    switch (*cmd) {
        case Command::new_thread: navigate("#/threads/new"); return true;
        case Command::search: open_search(); return true;
        case Command::toggle_rail: toggle_rail(); return true;
        case Command::settings: navigate("#/settings"); return true;
        case Command::help: show_shortcuts(); return true;
        case Command::archive: {
            const ThreadEntry* te = entry(target);
            archive(target, !(te && te->archived));
            return true;
        }
        case Command::mark_unread:
            if (!chat) mark_unread(target);
            return true;
        case Command::remove:
            if (chat) ask_leave_chat(target);
            else ask_delete(target);
            return true;
        case Command::next:
        case Command::previous: return false;  // the list moves its selection itself
        case Command::open:
            if (!target.empty()) open_thread(target);
            return true;
        case Command::back:
            if (search_open()) {
                close_search();
                return true;
            }
            if (!open_id_.empty() || route_.name == RouteName::new_thread || route_.name == RouteName::chat) {
                navigate(view_hash(view_));
                window_->tree().set_focus(&list_->list());
                return true;
            }
            return false;
    }
    return false;
}

}  // namespace wb::shell
