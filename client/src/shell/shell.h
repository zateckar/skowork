// The inside of the native window (add-native-ui group 3): the state the screens show, the calls to the host and
// the element tree. One Shell exists per open window; MainWindow creates it on open and drops it on close.
#pragma once

#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "api_connectors/token_draft.h"
#include "shell/feed.h"
#include "shell/host_link.h"
#include "shell/inbox_model.h"
#include "shell/main_window.h"
#include "shell/routes.h"
#include "ui/elements.h"
#include "ui/list.h"
#include "ui/richtext.h"
#include "ui/text16.h"
#include "ui/window.h"

namespace wb::shell {

class NavPane;
class ThreadListPane;
class SearchView;
class ThreadPane;
class RailPane;
class ChatsView;
class PagePane;
class StatusBarView;
class BannerArea;
class DictateControl;

// Responsive breakpoints of the web UI (DIPs).
inline constexpr float kNavWide = 216, kNavCompact = 56, kNavCompactBelow = 720;
inline constexpr float kSinglePaneBelow = 860, kRailDockedFrom = 1180, kRailWidth = 280;
inline constexpr float kListMin = 240, kListMax = 420, kListDefault = 300;

class Shell {
public:
    Shell(HostLink& host, const MainWindow::Options& options, std::function<void()> on_closed);
    ~Shell();
    Shell(const Shell&) = delete;
    Shell& operator=(const Shell&) = delete;

    bool ok() const { return window_ != nullptr; }
    ui::Window& window() { return *window_; }
    ui::Ui& ui() { return window_->ui(); }
    std::u16string t(std::string_view key, const ui::Params& params = {}) { return ui().strings.get(key, params); }
    std::u16string count(std::string_view key, long long n, ui::Params params = {}) {
        return ui().strings.count(key, n, std::move(params));
    }
    long long now() const;
    char decimal() const { return language_ == "cs" || language_ == "de" ? ',' : '.'; }  // the language's separator

    // --- navigation
    void navigate(const std::string& hash);  // unknown routes go to the Inbox
    const Route& route() const { return route_; }
    void bring_to_front();

    // --- host
    using Done = std::function<void(Result<json>)>;
    void call(const std::string& method, const json& params, Done done = {});
    void emit(const std::string& event, const json& data);

    // --- state the views read
    const json& status() const { return status_; }
    const Counts& counts() const { return counts_; }
    const std::vector<Folder>& folders() const { return folders_; }
    const ViewId& view() const { return view_; }
    const std::vector<ThreadEntry>& entries() const { return entries_; }
    bool list_loading() const { return list_loading_; }
    bool list_has_more() const { return !next_cursor_.empty(); }
    std::set<Group>& collapsed() { return collapsed_; }
    const ThreadEntry* entry(const std::string& id) const;
    const std::string& open_id() const { return open_id_; }
    // The list entry the reading pane shows: the open Thread, or "chat:<id>" on a Chat route.
    std::string shown_entry() const;
    std::optional<FeedSnapshot>& feed() { return feed_; }
    LiveText& live() { return live_; }
    FeedFold& fold() { return fold_; }
    // The feed repaints for a live update: streamed text at most 30 times a second, other updates at once.
    void feed_changed(bool streamed);
    // Why the open Thread's feed could not be loaded: "" (loaded or loading), "not_found" or the error message.
    const std::string& feed_error() const { return feed_error_; }
    bool rail_open() const { return rail_open_; }
    bool rail_docked() const { return rail_docked_; }
    bool single_pane() const { return single_pane_; }
    // Context usage of the open Thread (threads.context), null until loaded.
    const json& context() const { return context_; }

    // --- actions (views and shortcuts)
    void load_more();
    void reload_list();
    void open_thread(const std::string& id);
    void toggle_rail();
    void archive(const std::string& id, bool archived);
    void mark_unread(const std::string& id);
    void pin(const std::string& id, bool pinned);
    void ask_delete(const std::string& id);
    void ask_rename(const std::string& id);
    void move_to(const std::string& id, const std::string& folder_id);
    // Leave Chat from a Chat entry's menu (add-chats-in-inbox D3), after asking.
    void ask_leave_chat(const std::string& entry_id);
    void show_thread_menu(const std::string& id, const ui::Rect& anchor);
    void ask_new_folder();
    // New Chat (fold-chats-into-inbox D2): pick colleagues; one makes a direct Chat, two or more a named team Chat.
    void ask_new_chat();
    // The user's own busy state (Own Presence), switched from the status bar (fold-chats-into-inbox D3).
    void set_busy(bool busy);
    // Dictation (add-dictation D3): one recording per window, owned by a composer ("thread:<key>" or "chat:<id>").
    // Stop inserts the text into the owner's field at the caret; the text is dropped when the owner is gone.
    enum class DictationPhase { idle, starting, recording, transcribing };
    DictationPhase dictation_phase() const { return dictation_.phase; }
    const std::string& dictation_owner() const { return dictation_.owner; }
    long long dictation_started() const { return dictation_.started; }
    void start_dictation(const std::string& owner, std::weak_ptr<int> alive, std::function<ui::TextField*()> field);
    void stop_dictation();
    void cancel_dictation();
    void discard_dictation();  // the host already discarded the recording (policy turned dictation off)
    // Escape in a composer: cancels the owner's recording; false when it has none.
    bool cancel_dictation_of(const std::string& owner);
    void watch_dictation(DictateControl* c);
    void unwatch_dictation(DictateControl* c);
    // A dialog with one text field (<id>-field) and Save / Cancel (<id>-save, <id>-cancel); `save` gets the trimmed,
    // non-empty text.
    void ask_text_value(const std::string& id, const std::u16string& title, const std::u16string& label,
                        const std::u16string& initial, std::function<void(const std::string&)> save);
    // The folder's own menu (Rename, Delete), from the Thread list's header in a folder view.
    void show_folder_menu(const std::string& folder_id, const ui::Rect& anchor);
    void ask_rename_folder(const std::string& folder_id);
    void ask_delete_folder(const std::string& folder_id);
    void load_feed();
    // Fork From Message: a new Thread with the history up to the message, opened.
    void fork(const std::string& thread_id, const std::string& run_id, long long seq);
    // Error Banners With Remedy.
    void retry(const std::string& thread_id);
    void compact(const std::string& thread_id);
    void load_context();
    void refresh_app_status() { load_status(); }
    void apply_language(const std::string& lang) { set_language(lang); }
    // Show work details (client/work-inbox Work Details Visibility): reasoning and tool-call cards for every Run.
    bool show_work_details() const { return show_work_details_; }
    void set_show_work_details(bool on);
    // A local time of day (unix seconds) in the window's language.
    std::u16string clock(long long unix_seconds);
    // A moment (unix ms) in the window's language: the time today, the date and time otherwise.
    std::u16string moment(long long unix_ms);
    bool copy_text(const std::string& utf8);  // to the clipboard
    void sign_in();
    // Full-Text Search in the Thread list's place (Ctrl+F); Escape closes it.
    void open_search();
    void close_search();
    bool search_open() const;
    // Keyboard shortcut help (Ctrl+/).
    void show_shortcuts();
    // Why new Runs cannot start now (the agent is unavailable or its start gate is closed); "" when they can.
    std::string runs_blocked() const;
    // Signed in and registered but disconnected (client/control-channel Offline Operation): new requests wait in
    // their Thread instead of being refused.
    bool offline() const;
    // The width of the Knowledge view's list pane, kept while the window is open (improve-knowledge-view D1).
    float knowledge_list_w = 360;
    std::string knowledge_section = "sources";  // the Knowledge view's last section in this window
    // Save as note (add-save-as-note D3, D4): the draft the Chats view hands to the Knowledge view, which opens it in
    // the editor.
    json knowledge_draft = nullptr;
    // The files the user picks for the composer (the system Open dialog, or the tests' picker).
    std::vector<std::wstring> pick_files();
    // The system folder picker; empty when cancelled. Tests replace it (Options::pick_folder).
    std::wstring pick_folder();
    // Images a tool stored in the open Thread (add-image-generation: Images In The Thread Feed). Each is loaded from
    // the host once per window and decoded off the window's thread; `done` runs on the window's thread when it is
    // ready or failed. image() is null until then.
    struct LoadedImage {
        std::string bytes;                      // the stored file
        com_ptr<IWICBitmapSource> bitmap;       // decoded, at most 2048 px on the longer side
        std::string error;
        bool done = false;
        std::vector<std::function<void()>> waiting;
    };
    void load_image(const std::string& thread_id, const FileRef& file, std::function<void()> done);
    const LoadedImage* image(const std::string& id) const;
    void show_image(const FileRef& file);  // Open: the image at full size in a dialog
    void save_image(const FileRef& file);  // Save as
    void announce(const std::u16string& text) { window_->announce(text); }
    // A host call failed: announced like the web UI's alert region.
    void alert(const Error& e) { announce(t("common.error_prefix") + u" " + ui::to_utf16(e.message)); }
    // The views rebuild what changed.
    void refresh_nav();
    void refresh_list();
    void refresh_thread();
    void refresh_status();

    NavPane& nav() { return *nav_; }
    ThreadListPane& list() { return *list_; }
    ThreadPane& thread() { return *thread_; }

private:
    void build();
    void apply_layout(ui::Size viewport);
    void apply_route();
    void load_status();
    void load_folders(std::function<void()> then = {});
    void set_entries(std::vector<ThreadEntry> entries);
    void on_inbox_changed(const json& data);
    void on_run_event(const json& data);
    // The reading pane of a Chat route, or the directory of the Chats list (add-chats-in-inbox D3, D4).
    void update_chat_slot();
    void set_language(const std::string& lang);
    bool on_key(const ui::KeyEvent& e);
    void dictation_changed();  // the Dictate controls show the phase, the elapsed time and the availability
    void dictation_tick();     // every second while recording

    struct DictationRun {
        std::string owner;
        DictationPhase phase = DictationPhase::idle;
        long long started = 0;
        std::weak_ptr<int> alive;
        std::function<ui::TextField*()> field;
        UINT_PTR timer = 0;
    };
    DictationRun dictation_;
    std::vector<DictateControl*> dictate_controls_;

    HostLink& host_;
    MainWindow::Options options_;
    std::function<void()> on_closed_;
    std::shared_ptr<int> alive_ = std::make_shared<int>(0);
    std::unique_ptr<ui::Window> window_;

    Route route_;
    json status_ = json::object();
    Counts counts_;
    std::vector<Folder> folders_;
    ViewId view_ = "inbox";
    std::vector<ThreadEntry> entries_;
    std::string next_cursor_;
    bool list_loading_ = false;
    uint64_t list_generation_ = 0;
    std::set<Group> collapsed_ = collapsed_by_default();
    std::string open_id_;  // the Thread shown in the thread pane ("" none)
    std::optional<FeedSnapshot> feed_;
    LiveText live_;
    FeedFold fold_;
    RepaintLimit repaint_;
    std::unique_ptr<RepaintTimer> repaint_timer_;  // the feed's streamed repaints (feed.h RepaintTimer)
    std::string feed_error_;
    std::string feed_in_flight_;  // the Thread whose snapshot is being loaded
    bool feed_again_ = false;
    uint64_t feed_generation_ = 0;
    json context_ = nullptr;
    bool rail_open_ = true;  // the user's choice (Ctrl+B); the drawer starts closed
    bool drawer_open_ = false;
    bool rail_docked_ = true, single_pane_ = false, nav_compact_ = false;
    std::string language_;
    bool show_work_details_ = false;
    std::map<std::string, std::shared_ptr<LoadedImage>> images_;

    NavPane* nav_ = nullptr;
    BannerArea* banners_ = nullptr;
    ui::Node* inbox_ = nullptr;  // list | splitter | thread | rail
    ThreadListPane* list_ = nullptr;
    ui::Splitter* splitter_ = nullptr;
    ThreadPane* thread_ = nullptr;
    ui::ScrollView* chat_slot_ = nullptr;  // in the thread pane's place for Chats
    ChatsView* chats_ = nullptr;           // its content, if any
    RailPane* rail_ = nullptr;
    ui::Node* drawer_layer_ = nullptr;
    RailPane* drawer_ = nullptr;
    PagePane* page_ = nullptr;
    StatusBarView* status_bar_ = nullptr;
};

// ---- views ----------------------------------------------------------------------------------------------------

// The navigation pane: New thread, the smart views with counts, folders, and Status, Settings, About.
class NavPane : public ui::Box {
public:
    explicit NavPane(Shell& shell);
    void rebuild();
    void set_compact(bool compact);

private:
    Shell& shell_;
    bool compact_ = false;
};

// Notices above the content (derive.h banners) with their remedy.
class BannerArea : public ui::Node {
public:
    explicit BannerArea(Shell& shell);
    void rebuild();

private:
    Shell& shell_;
    std::vector<std::string> dismissed_;
};

// The Thread list of the current view: date groups, pins, unread and state marks; Enter or a click opens a Thread.
class ThreadListPane : public ui::Box {
public:
    explicit ThreadListPane(Shell& shell);
    void rebuild();  // the rows changed
    void select_thread(const std::string& id);
    std::string selected_thread() const;
    bool on_key(const ui::KeyEvent& e) override;
    ui::ListView& list() { return *list_; }
    const std::vector<ListRow>& rows() const { return rows_; }
    // Shows the search in place of the list, or the list again.
    void set_search(bool open);
    bool search_open() const;
    SearchView& search() { return *search_; }

private:
    void build_row(size_t index, ui::ListRow& row);
    Shell& shell_;
    ui::Label* title_ = nullptr;
    ui::Button* folder_button_ = nullptr;  // folder views only
    ui::Button* search_button_ = nullptr;
    SearchView* search_ = nullptr;
    ui::Label* empty_ = nullptr;
    ui::ListView* list_ = nullptr;
    std::vector<ThreadEntry> entries_;
    std::vector<ListRow> rows_;
};

// Full-Text Search: the query field, then the hits with their matches in bold. It searches as the user types
// (debounced); opening a hit opens its Thread at the matching message.
class SearchView : public ui::Node {
public:
    explicit SearchView(Shell& shell);
    ~SearchView() override;
    void reset();  // an empty query, before the search opens again
    ui::TextField& field() { return *field_; }

private:
    void changed();
    void query(const std::string& q);
    void show(const json& hits);
    Shell& shell_;
    ui::TextField* field_ = nullptr;
    ui::Label* status_ = nullptr;
    ui::Node* results_ = nullptr;
    UINT_PTR timer_ = 0;
    uint64_t generation_ = 0;
    std::shared_ptr<int> alive_ = std::make_shared<int>(0);
};

// The Thread feed (Streaming Thread Feed, Markdown Rendering): messages, reasoning blocks, tool-call cards, Action
// items and Run activity in order. update() reconciles it with the shell's snapshot and streaming text: elements of
// unchanged items stay (with their text layouts, expansion and focus), a streaming message only re-parses its tail.
class FeedView : public ui::ScrollView {
public:
    FeedView(Shell& shell, std::string thread_id);
    void update();
    const std::string& thread_id() const { return thread_; }
    size_t rebuilt_last() const { return rebuilt_; }  // items built anew by the last update (tests)
    size_t rebuilt_total() const { return rebuilt_total_; }  // items built anew by all updates so far (tests)
    // The message the route points at (a search hit): scrolled to and highlighted briefly; "" when none is.
    const std::string& highlighted() const { return highlighted_; }

private:
    void jump();  // to the route's message once it is in the feed
    struct Slot {
        std::string key, sig;
        ui::MarkdownView* markdown = nullptr;  // a streaming or stored model message
        ui::Label* text = nullptr;             // a reasoning block's text
    };
    std::unique_ptr<ui::Node> build(const FeedItem& item, Slot& slot);
    // Work Details Visibility: the items shown (reasoning, tool cards and step activity only where details are on), a
    // plain step label for the working line, and Show details after each finished Run with hidden work.
    std::vector<FeedItem> visible(std::vector<FeedItem> items) const;
    void add_outputs(ui::Node& wrap, const FeedItem& it, bool citations);
    // A numbered list of links to passages, under an answer or a tool card (Cited Sources In The Feed).
    void add_citations(ui::Node& wrap, const std::vector<Citation>& citations, const std::string& key, const char* list_prefix,
                       const char* item_prefix);
    Shell& shell_;
    std::string thread_;
    std::vector<Slot> slots_;  // parallel to the children
    std::set<std::string> expanded_;
    std::set<std::string> details_;  // Runs whose details the user opened
    size_t rebuilt_ = 0;
    size_t rebuilt_total_ = 0;
    std::optional<MessageRef> jumped_;
    std::string highlighted_;
    UINT_PTR jump_timer_ = 0, highlight_timer_ = 0;
    std::shared_ptr<int> alive_ = std::make_shared<int>(0);
};

// The composer (Composer, Queue Or Steer While Running) of a Thread, or of a new Thread (key "new"): the message
// field with its draft, attachments from the picker, paste or drop, the `/` skill list, the queued messages, and
// Send, Queue, Steer now and Stop. The draft text is saved shortly after each edit and when the composer goes.
class ComposerView : public ui::Box {
public:
    ComposerView(Shell& shell, std::string thread_key);
    ~ComposerView() override;
    const std::string& thread_key() const { return key_; }
    void update();  // the snapshot or the status changed: the active Run, the queued messages, the blocked notice
    void attach(const std::vector<std::wstring>& paths);
    void pick();
    void send(bool steer);
    void stop();
    ui::TextField& field() { return *field_; }
    // A message was sent; for a new Thread, the window opens it.
    std::function<void(const std::string& thread_id)> on_sent;

private:
    struct Attachment {
        std::string id, name;
        long long size = 0;
    };
    struct Skill {
        std::string name, description;
    };
    void text_changed();
    void save_soon();
    void save_now();
    bool field_key(const ui::KeyEvent& e);
    void choose_skill(size_t index);
    void show_error(const std::u16string& text);
    void rebuild_queued();
    void rebuild_attachments();
    void rebuild_skills();
    void rebuild_bar();
    std::optional<std::string> skill_query() const;  // the text after a leading `/` while it is one word
    std::vector<Skill> skill_matches() const;

    Shell& shell_;
    std::string key_;
    std::shared_ptr<int> alive_ = std::make_shared<int>(0);
    ui::Node* queued_ = nullptr;
    ui::Label* blocked_ = nullptr;
    ui::Node* attachments_ = nullptr;
    ui::Node* skills_ = nullptr;
    ui::TextField* field_ = nullptr;
    ui::Node* bar_ = nullptr;
    ui::Label* error_ = nullptr;
    std::u16string text_;  // the field's text as of the last edit (the field's window may be gone when saving)
    bool dirty_ = false;   // text_ is not saved yet
    std::vector<Attachment> files_;
    size_t attaching_ = 0;
    std::optional<std::vector<Skill>> skill_list_;  // loaded when `/` is first typed
    size_t skill_index_ = 0;
    UINT_PTR save_timer_ = 0;
    bool busy_ = false;
    std::string queued_sig_, bar_state_;  // what the queued list and the bar show
};

// The open Thread: its title and actions, the error banner, the feed and the composer; or the hint to select a
// Thread.
class ThreadPane : public ui::Box {
public:
    explicit ThreadPane(Shell& shell);
    void rebuild();
    void update_feed();      // a live event changed the snapshot
    void update_composer();  // the status changed (new Runs blocked or not)
    FeedView* feed() { return feed_; }
    ComposerView* composer() { return composer_; }

private:
    void rebuild_banner();
    Shell& shell_;
    ui::Node* banner_ = nullptr;
    FeedView* feed_ = nullptr;
    std::unique_ptr<FeedView> kept_feed_;
    ComposerView* composer_ = nullptr;
    std::unique_ptr<ComposerView> kept_composer_;
    std::string announced_;  // the failed Run whose banner was announced
};

// The Activity rail: subagent Runs of the open Thread, grouped by the message that started them. An entry loads its
// child Run's transcript the first time it is expanded.
class RailPane : public ui::Box {
public:
    RailPane(Shell& shell, std::string id, bool drawer);
    void rebuild();

private:
    struct Transcript {
        bool loaded = false;
        std::u16string error;
        std::vector<std::u16string> lines;
    };
    void load(const std::string& run_id);
    void fill(const std::string& run_id);
    Shell& shell_;
    bool drawer_;
    std::shared_ptr<int> alive_ = std::make_shared<int>(0);
    std::set<std::string> expanded_;                   // run ids, kept across rebuilds
    std::map<std::string, Transcript> transcripts_;    // run id → loaded (or loading) transcript
    std::map<std::string, ui::Node*> bodies_;          // run id → entry body of the current build
};

// The Skill editor (client/skills Skill Authoring, Save Thread As Skill, Skill Test Run, Skill Publishing; add-skills
// design D10): the frontmatter and the instructions, checked as they change; the personal data a draft replaced, each
// with Keep; Save, Test and Delete; and for a saved Skill its files and publishing. Its text fields live as long as the
// route, so page rebuilds keep what the user typed.
class SkillEditor : public ui::Node {
public:
    SkillEditor(Shell& shell, const Route& route);
    const std::string& key() const { return key_; }

private:
    void load();
    void set_source(const json& s);
    json source() const;
    void validate();
    void save();
    void ask_delete();
    void restore();  // Restore previous version (add-agent-created-skills D12)
    void test();
    void keep(size_t index);
    void add_files();
    void remove_file(const std::string& path);
    void publish();
    void ask_withdraw(const std::string& version);
    void load_publishing();
    void show_status(const std::u16string& text, ui::Paint color = ui::Paint::text_secondary);
    void rebuild_replaced();
    void rebuild_files();
    void rebuild_publishing();
    void update_buttons();
    void menu(ui::Button* anchor, const std::string& id, std::vector<std::pair<std::string, std::u16string>> options,
              std::function<void(const std::string&)> pick);

    Shell& shell_;
    std::string key_, original_, draft_thread_;
    ui::Label* status_ = nullptr;
    ui::Node* replaced_ = nullptr;
    ui::TextField* frontmatter_ = nullptr;
    ui::TextField* body_ = nullptr;
    ui::Label* validation_ = nullptr;
    ui::Button* save_ = nullptr;
    ui::Button* test_ = nullptr;
    ui::Button* delete_ = nullptr;
    ui::Button* restore_ = nullptr;
    bool has_previous_ = false;  // the Skill has a kept version (skills.source "previous")
    ui::TextField* test_input_ = nullptr;
    ui::Node* files_section_ = nullptr;
    ui::Node* files_ = nullptr;
    ui::Button* folder_ = nullptr;
    ui::Node* publish_section_ = nullptr;
    ui::Button* scope_ = nullptr;
    ui::TextField* audience_ = nullptr;
    ui::Label* audience_hint_ = nullptr;
    ui::Label* publish_result_ = nullptr;
    ui::Node* published_ = nullptr;
    json replaced_list_ = json::array(), files_list_ = json::array(), published_list_ = json::array();
    std::vector<std::string> scopes_{"shared"}, groups_;
    std::string folder_choice_ = "scripts", scope_choice_ = "shared";
    bool loaded_ = false, valid_ = false, busy_ = false;
    uint64_t generation_ = 0;
    std::shared_ptr<int> alive_ = std::make_shared<int>(0);
};

// The Automations editor (client/scheduler Automations Section): one form for every Trigger kind, with the fields of the
// chosen kind shown; the host validates and saves (automations.save, .delete).
class AutomationEditor : public ui::Node {
public:
    AutomationEditor(Shell& shell, const Route& route);
    const std::string& key() const { return key_; }

private:
    void load();
    void fill(const json& t);
    json definition() const;
    void show_kind();
    void save();
    void ask_delete();
    void show_status(const std::u16string& text, ui::Paint color = ui::Paint::text_secondary);
    void menu(ui::Button* anchor, const std::string& id, std::vector<std::pair<std::string, std::u16string>> options,
              std::function<void(const std::string&)> pick);

    Shell& shell_;
    std::string key_, id_;
    std::string kind_ = "schedule", thread_mode_ = "new", session_event_ = "logon";
    std::vector<std::string> kinds_{"schedule", "file", "session"};
    ui::Label* status_ = nullptr;
    ui::TextField* name_ = nullptr;
    ui::Button* kind_button_ = nullptr;
    ui::Node* schedule_ = nullptr;
    ui::TextField* cron_ = nullptr;
    ui::TextField* interval_ = nullptr;
    ui::TextField* zone_ = nullptr;
    ui::Node* file_ = nullptr;
    ui::TextField* folder_ = nullptr;
    ui::TextField* pattern_ = nullptr;
    ui::Node* session_ = nullptr;
    ui::Button* event_button_ = nullptr;
    ui::Node* source_ = nullptr;
    ui::TextField* conditions_ = nullptr;
    ui::TextField* prompt_ = nullptr;
    ui::TextField* skill_ = nullptr;
    ui::Button* mode_button_ = nullptr;
    ui::Checkbox* notify_ = nullptr;
    ui::Checkbox* enabled_ = nullptr;
    ui::Button* save_ = nullptr;
    ui::Button* delete_ = nullptr;
    bool busy_ = false;
    std::shared_ptr<int> alive_ = std::make_shared<int>(0);
};

// The Knowledge view (client/knowledge-graph Knowledge View, Knowledge Curation, Sharing Knowledge Items; add-knowledge-
// sharing design D4; improve-knowledge-view): a header with the search field and the Add menu, then a list pane (Source
// tabs with counts, two-line rows, or the search results) and a reader (the open item with its actions, a More menu and
// collapsed Details; the note editor; Manage imports). Below 900 DIPs only one pane shows and the reader offers Back.
// The skeleton is built once, so the search field and the list's scroll position survive updates. The host does the
// work (knowledge.search, .browse, .item, .status, .document.*, .folder.*, .note.*, .fact.*, .entity.rename,
// .item.forget, .exclusion.add, .sharing, .share, .unshare, .share_folder, .unshare_folder). The page pane keeps it
// across rebuilds. Sections (task 10.1): Sources (the Source tabs), Knowledge (entities), Review (possible
// inconsistencies) and Sharing (shared items and folders), each with its list; the reader also shows a fact with its
// evidence, an entity, a review with its decisions and a change report. A reader opened from another one (a fact from
// an item, its excerpt's item from the fact) keeps the way back: Back returns through them to the list.
class KnowledgeView : public ui::Node {
public:
    explicit KnowledgeView(Shell& shell);
    void changed();  // knowledge.changed: the list, the item and the sharing state load again

    static constexpr float kListDefault = 360, kListMin = 280, kListMax = 560, kSinglePaneBelow = 900;

private:
    class Split;
    enum class Reader { empty, item, editor, imports, fact, entity, review, change };
    // A reader to return to with Back.
    struct Place {
        Reader mode = Reader::empty;
        long long id = 0;
        std::string selected;
        json cited;
        float scroll = 0;  // the reader's scroll position
    };

    void rebuild();
    void rebuild_messages();
    void rebuild_tabs();
    void rebuild_list();
    void rebuild_reader();
    void load_list();
    void load_sharing();
    void open_item(long long id, const std::string& key);
    void open_note(long long note_id);  // the note editor (0: a new note)
    void open_draft(const json& draft);  // Save as note: the editor with {title, text, origin}; nothing is stored yet
    void save_mail_as_note(long long item_id);
    void open_origin(const json& origin);  // the mail item or the Chat a note was saved from
    void close_reader();                // Back: the previous reader, or the list on a narrow window
    // Workspace sections and readers (task 10.1).
    void set_section(const std::string& section);
    void load_section(bool more = false);  // more: the next page of the shown list
    void build_knowledge_list();
    void build_review_list();
    void more_button(const json& page);    // Load more when the page has a next one
    void load_fact_categories(long long fact_id);
    void build_fact_categories(ui::Node& parent, long long fact_id);
    void list_chip(ui::Node& parent, const std::string& id, const std::u16string& label, bool on, std::function<void()> f);
    void rebuild_sections();
    void build_section_list();
    void push_place();                         // before a reader opened from the current one
    void restore(const Place& p);
    void open_fact(long long fact_id);
    void open_entity(long long entity_id);
    void open_review(long long review_id);
    void open_change(long long report_id);
    void open_cited(const json& citation);     // the item of an excerpt, with the excerpt shown
    void load_detail(const std::string& method, const json& params, Reader mode, long long id);
    void load_item_changes(long long item_id);
    void refresh_item(long long id);
    void ask_decision(long long review_id, const std::string& decision);
    void build_fact(ui::Node& parent);
    void build_entity(ui::Node& parent);
    void build_review(ui::Node& parent);
    void build_change(ui::Node& parent);
    void build_item_jumps(ui::Node& parent);
    void build_item_changes(ui::Node& parent, long long id);
    void build_citation(ui::Node& parent, const json& c, const std::string& id);
    void focus(const std::string& automation_id);
    void search();
    void clear_search();
    void show_add_menu();
    void show_more_menu();
    void import_files();
    void import_folder();
    void save_note();
    // `copied`: the extra line for a note saved from mail or a Chat (add-save-as-note D4).
    void ask_share(const std::string& id, const std::u16string& title, const std::u16string& size,
                   std::function<void(const std::vector<std::string>&)> share, const std::u16string& copied = {});
    void ask_confirm(const std::string& id, const std::u16string& title, const std::u16string& body,
                     const std::u16string& action, std::function<void()> confirmed);
    void act(const std::string& method, const json& params, std::function<void(const json&)> done = {},
             std::function<bool()> current = {});
    void build_item(ui::Node& parent);
    void build_details(ui::Node& parent, long long id);
    void build_editor(ui::Node& parent);
    void build_imports(ui::Node& parent);
    ui::Label* text(ui::Node& parent, const std::u16string& s, ui::Paint color = ui::Paint::text, const std::string& id = {});
    ui::Button* button(ui::Node& parent, const std::string& id, const std::u16string& label, std::function<void()> f,
                       ui::Button::Kind kind = ui::Button::Kind::standard, char16_t glyph = 0);
    std::u16string size_text(long long bytes);

    Shell& shell_;
    // The skeleton.
    ui::TextField* search_ = nullptr;
    ui::Button* add_ = nullptr;
    ui::Node* messages_ = nullptr;   // sharing off, notice, error
    ui::Node* sections_ = nullptr;   // Sources, Knowledge, Review, Sharing
    ui::Node* tabs_ = nullptr;       // the Source tabs with counts
    Split* split_ = nullptr;
    ui::Node* list_pane_ = nullptr;
    ui::Node* list_head_ = nullptr;  // tabs and Manage imports, or the results header
    ui::Node* list_ = nullptr;       // the scrolling rows
    ui::Node* reader_ = nullptr;     // the scrolling reader
    ui::Node* back_ = nullptr;       // the reader's Back button (narrow windows)
    ui::Button* more_ = nullptr;     // the open item's More button

    std::string tab_ = "files";  // the Source tab
    std::string section_ = "sources";
    json entities_ = nullptr;      // knowledge.entities (Knowledge, Entities)
    json categories_ = nullptr;    // knowledge.categories (Knowledge)
    json facts_ = nullptr;         // knowledge.facts (Knowledge, a category or all)
    std::string knowledge_scope_;  // Knowledge list: "" (categories), "all", "c<id>" (0: Uncategorized), "entities"
    std::string review_kind_ = "inconsistencies";  // Review list: inconsistencies, changes, categories
    json review_changes_ = nullptr;  // knowledge.changes (Review, changed evidence)
    json suggestions_ = nullptr;     // knowledge.category.suggestions (Review)
    json fact_categories_ = nullptr; // knowledge.fact.categories of the open fact
    float pending_scroll_ = -1;      // the reader's scroll to restore once a reader opened by Back is shown
    float list_scroll_ = 0;          // the list's position when the reader covered it (narrow window)
    json reviews_ = nullptr;       // knowledge.reviews (Review)
    json detail_ = nullptr;        // the open fact, entity, review or change report
    long long detail_id_ = 0;
    json item_changes_ = nullptr;  // knowledge.changes of the open item
    long long item_changes_of_ = 0;
    json cited_ = nullptr;         // the excerpt the open item was opened at
    json review_history_ = nullptr;
    std::vector<Place> history_;   // readers to return to
    size_t detail_request_ = 0, section_request_ = 0, changes_request_ = 0;
    std::u16string query_, shown_query_, note_title_, note_text_;
    json results_ = nullptr;   // knowledge.search
    json items_ = nullptr;     // knowledge.browse of the tab
    json counts_ = nullptr;    // knowledge.browse counts per Source
    json item_ = nullptr;      // knowledge.item of the open item
    json sharing_ = nullptr;   // knowledge.sharing
    json folders_ = nullptr;   // knowledge.status folders and documents
    Reader reader_mode_ = Reader::empty;
    std::string selected_;      // the row of the open item ("<id>" or "n<note id>")
    bool details_open_ = true;  // the extracted knowledge section of the open item
    long long note_id_ = 0;    // the note being edited (0: new)
    json note_origin_ = nullptr;  // the origin of a Save as note draft, stored with the new note
    bool save_as_note_ = false;   // knowledge.save_as_note
    bool busy_ = false;
    std::u16string error_, notice_, file_source_error_;
    size_t file_status_request_ = 0;
    size_t sharing_request_ = 0;
    size_t item_request_ = 0;
    size_t list_request_ = 0;
    size_t search_request_ = 0;
    std::shared_ptr<int> alive_ = std::make_shared<int>(0);
};

// The Chats in the reading pane (client/user-messaging Chats View, Own Presence; add-user-messaging design D4,
// add-chats-in-inbox D3, D4). The Chats themselves are Inbox entries in the list. In `conversation` mode it shows one
// Chat: its messages as Markdown with sender, time and a pending marker, the composer, rename, add members, leave and
// Save as note. New Chat (Shell::ask_new_chat) starts Chats; busy is in the status bar (fold-chats-into-inbox D2, D3).
// The host does the work (messaging.*).
// The Dictate button of a composer (add-dictation D3): the microphone; while recording, Stop with the elapsed time and
// Cancel; while transcribing, a progress line. Hidden while policy.dictation.mode is off, disabled with the reason as
// its name and tooltip while dictation is not available.
class DictateControl : public ui::Node {
public:
    DictateControl(Shell& shell, std::string id, std::string owner, std::weak_ptr<int> alive, std::function<ui::TextField*()> field);
    ~DictateControl() override;
    void update();  // the phase, the time or the availability changed

private:
    Shell& shell_;
    std::string id_;  // the microphone button; -stop, -time, -cancel and -progress while it works
    std::string owner_;
    std::weak_ptr<int> alive_;
    std::function<ui::TextField*()> field_;
    std::string shown_;  // what the children show
    ui::Label* time_ = nullptr;
};

class ChatsView : public ui::Node {
public:
    ChatsView(Shell& shell, std::string chat);
    void changed(const json& data);  // messaging.changed: the status, and the open Chat when it is the one that changed
    const std::string& open_chat() const { return open_; }

private:
    void rebuild();
    void load_list();
    void open(const std::string& id);
    void send();
    void ask_add(const std::string& id, const json& members);  // Add members with the people picker
    void ask_leave(const std::string& id, const std::u16string& title);
    void ask_save_note(const std::string& id);  // Save as note with the period (add-save-as-note D3)
    void act(const std::string& method, const json& params, std::function<void(const json&)> done = {});
    void build_conversation(ui::Node& parent);
    std::u16string title_of(const json& chat) const;
    std::u16string presence_text(const std::string& presence);
    std::u16string when(const std::string& iso);
    ui::Button* button(ui::Node& parent, const std::string& id, const std::u16string& label, std::function<void()> f,
                       ui::Button::Kind kind = ui::Button::Kind::standard, char16_t glyph = 0);

    Shell& shell_;
    json status_ = nullptr;    // messaging.chats: enabled, connected, me, busy, unread, chats
    json messages_ = nullptr;  // messaging.messages of the open Chat
    std::string open_;         // the open Chat
    std::u16string error_;
    std::map<std::string, std::u16string> drafts_;  // by Chat
    ui::TextField* composer_ = nullptr;              // the open Chat's message field
    bool save_as_note_ = false;                     // knowledge.save_as_note
    bool busy_ = false;
    std::shared_ptr<int> alive_ = std::make_shared<int>(0);
};

// Status, Settings and About. Each page loads its own data (storage usage, settings, version) when it is entered.
class PagePane : public ui::ScrollView {
public:
    explicit PagePane(Shell& shell);
    void rebuild();
    // The host's Triggers changed (automations.changed): the Automations section and a Trigger's page load again.
    void automations_changed();
    // The mailbox connection or sync changed (mail.changed): the Mail and calendar section loads again.
    void mail_changed();
    // The knowledge index changed (knowledge.changed): the Knowledge sources section loads again.
    void knowledge_changed();
    // Meeting capture changed (meetings.changed): the Meetings section loads again.
    void meetings_changed();
    // Local apps changed (apps.changed): the Apps view loads again.
    void apps_changed();

private:
    void load();
    void build_status();
    void build_settings();
    void build_connections(ui::Node& card);
    void build_privacy(ui::Node& card);
    void build_policy(ui::Node& card);
    void build_files(ui::Node& card);
    void load_policy();
    void build_skills(ui::Node& card);
    void load_skills(bool reload);
    void build_automations(ui::Node& card);
    void load_automations();
    void build_mail(ui::Node& card);
    void load_mail();
    // The API connectors of the Connectors section (client/api-connectors): the listing, and the token entry.
    void load_connectors();
    void build_api_connectors(ui::Node& card);
    void build_knowledge(ui::Node& card);
    void load_knowledge();
    void find_knowledge(bool entities);
    void exclude_knowledge_files(std::vector<std::string> paths);  // item exclusions, one after another
    void build_meetings(ui::Node& card);
    void load_meetings();
    void build_apps();
    void load_apps();
    void open_app(const std::string& id, const std::u16string& title);
    void ask_delete_app(const std::string& id, const std::u16string& title);
    void ask_change_app(const std::string& id, const std::u16string& title);
    void open_entity(long long id);
    void ask_forget(const std::string& id, const std::u16string& what, const std::string& method, const json& params,
                    std::function<void()> done = {});
    void build_automation();
    void build_telemetry();
    void build_about();
    void ask_sign_out();
    void ask_wipe();
    ui::Node* card(const std::string& id, const std::u16string& title);
    void row(ui::Node& parent, const std::string& id, const std::u16string& key, const std::u16string& value,
             ui::Paint color = ui::Paint::text);
    ui::Label* text(ui::Node& parent, const std::u16string& s, ui::Paint color = ui::Paint::text, const std::string& id = {});
    ui::Button* button(ui::Node& parent, const std::string& id, const std::u16string& label, std::function<void()> f,
                       ui::Button::Kind kind = ui::Button::Kind::standard, char16_t glyph = 0);
    void choice(ui::Node& parent, const std::string& id, const std::u16string& label, const std::u16string& current,
                std::vector<std::pair<std::string, std::u16string>> options, std::function<void(const std::string&)> pick,
                bool can_change);
    void setting_switch(ui::Node& parent, const std::string& id, const std::u16string& label, bool on, bool managed,
                        std::function<void(bool)> change);
    void act(const std::string& id, const std::string& method, const json& params, std::function<void(const json&)> done = {});
    void error_line(ui::Node& parent, const std::string& id);

    Shell& shell_;
    RouteName shown_ = RouteName::inbox;
    std::string shown_hash_;
    json settings_ = nullptr, usage_ = nullptr, version_ = nullptr, telemetry_ = nullptr, skills_ = nullptr;
    std::u16string usage_error_, version_error_, telemetry_error_, copied_;
    size_t telemetry_shown_ = 100;  // records listed in the Telemetry viewer
    std::map<std::string, std::u16string> errors_;  // by action
    std::string busy_;                              // the action in progress
    long long verbose_minutes_ = 60;
    long long verbose_until_ = 0;  // unix seconds, until the host reports diagnostics in its status
    bool wiped_ = false;
    SkillEditor* editor_ = nullptr;
    std::unique_ptr<SkillEditor> kept_editor_;  // across page rebuilds while its route stays
    AutomationEditor* auto_editor_ = nullptr;
    std::unique_ptr<AutomationEditor> kept_auto_editor_;
    KnowledgeView* knowledge_view_ = nullptr;
    std::unique_ptr<KnowledgeView> kept_knowledge_view_;

    json automations_ = nullptr, automation_ = nullptr;
    json mail_ = nullptr;             // mail.status
    std::u16string mail_exclusion_;  // the exclusion being typed, kept across rebuilds
    json connectors_ = nullptr;      // GET api/v1/connectors: this user's connectors (never a token)
    std::string connector_editing_;  // the connector whose token field is open
    connectors::TokenDraft connector_token_;  // the token being typed; taken (and cleared) when it is sent
    json knowledge_ = nullptr;                                                // knowledge.status
    json knowledge_items_ = nullptr, knowledge_entities_ = nullptr;          // the last item and entity searches
    json knowledge_entity_ = nullptr;                                         // knowledge.entity of the chosen entity
    std::u16string knowledge_folder_, knowledge_exclusion_, knowledge_item_query_, knowledge_entity_query_;
    long long knowledge_merge_from_ = 0;  // the entity being merged into another
    json policy_ = nullptr;               // policy.summary
    json meetings_ = nullptr;             // meetings.status
    json apps_ = nullptr;                 // apps.list
    std::string meetings_open_;           // the meeting whose transcript is shown
    std::map<std::string, std::u16string> speaker_drafts_;  // "<meeting>-<speaker>" -> the name being typed
    std::u16string knowledge_merge_name_;
    std::shared_ptr<int> alive_ = std::make_shared<int>(0);
};

class StatusBarView : public ui::Box {
public:
    explicit StatusBarView(Shell& shell);
    void rebuild();

private:
    Shell& shell_;
};

}  // namespace wb::shell
