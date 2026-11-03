// The inbox state of Threads in app.db (client/work-inbox; add-work-inbox design D1, D8, D10, D12): read, archive and
// pin state, folders, origins, the smart-view queries with keyset paging, full-text search, drafts with their
// attachments, fork and the Thread part of the forget cascade. The preview, the activity time, the automatic
// un-archive and the search index are maintained by triggers of migration 300. Chats are Inbox entries too
// (add-chats-in-inbox design D1, D2): their state is in chat_inbox, which client/user-messaging keeps current, and the
// views list them with the Threads in one order. All methods are thread safe.
#pragma once

#include "base/json.h"
#include "base/result.h"
#include "base/time.h"
#include "runtime/types.h"
#include "storage/database.h"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wb::inbox {

// The id of a Chat's Inbox entry is this prefix and the Chat id (Chat Entries; add-chats-in-inbox design D2).
inline constexpr std::string_view kChatEntryPrefix = "chat:";
[[nodiscard]] inline bool is_chat_entry(std::string_view id) { return id.starts_with(kChatEntryPrefix); }

// One entry of the Thread list (Thread List Entries), or a Chat's entry (Chat Entries). has_action and running are
// derived from Runs and Action items.
struct ThreadEntry {
    std::string id, title, preview, last_activity, created_at;
    std::string kind = "thread";  // thread | chat
    long long unread_count = 0;   // a Chat's unread messages
    std::string chat_kind;        // a Chat's kind: direct | team
    std::string presence;         // the other user's Presence of a direct Chat
    std::string origin_kind = "user";  // user | trigger | remote | meeting | fork | system
    json origin_ref;                   // what the origin link opens (Origin Links)
    bool unread = false, archived = false, pinned = false;
    std::string folder_id;             // empty: the Inbox
    bool has_action = false, running = false;
    int open_actions = 0;
    std::string last_run_state;        // of the latest top-level Run; empty without one
};

struct Page {
    std::vector<ThreadEntry> entries;
    std::string next_cursor;  // empty on the last page
};

struct Counts {
    int unread = 0, action = 0, running = 0;
};

struct Folder {
    std::string id, name, created_at;
};

struct SearchHit {
    std::string thread_id, title;
    std::string run_id;   // with seq: the matching message; empty when the title matched
    long long seq = 0;
    std::string snippet;  // the matching text with [[ and ]] around the matched terms
    bool archived = false;
};

struct DraftAttachment {
    std::string id, name;
    long long size = 0;
};

struct Draft {
    std::string thread_key, text, updated_at;
    std::vector<DraftAttachment> attachments;
};

// Views (Smart Views and folders): "unread", "action", "running", "all", "archive", "inbox" (entries without a folder),
// "folder:<id>" and "chats" (every Chat that is not archived). Chats are listed in all of them but Action and Running.
[[nodiscard]] bool valid_view(const std::string& view);

class InboxStore {
public:
    static constexpr int kPageSize = 100;  // design D2
    static constexpr long long kMaxAttachmentBytes = 25ll * 1024 * 1024;  // design D12
    static constexpr int kMaxAttachments = 10;

    InboxStore(Database& db, const Clock& clock) : db_(db), clock_(clock) {}
    // Whether Chat entries are listed (policy.messaging.mode is not off); null: they are.
    void set_chats_listed(std::function<bool()> listed) { chats_listed_ = std::move(listed); }

    // ---- Views ----
    // Entries ordered pinned first, then by last activity (newest first). `cursor` is the next_cursor of the previous page.
    Result<Page> list(const std::string& view, const std::string& cursor = {}, int limit = kPageSize);
    [[nodiscard]] std::optional<ThreadEntry> entry(const std::string& thread_id);
    [[nodiscard]] Counts counts();

    // ---- Triage ----
    // set_archived, set_pinned and move also take a Chat entry id (Chat Entry Triage); a Chat's read state, title and
    // existence belong to client/user-messaging.
    Status set_unread(const std::string& thread_id, bool unread);
    Status set_archived(const std::string& thread_id, bool archived);
    Status set_pinned(const std::string& thread_id, bool pinned);
    Status rename(const std::string& thread_id, const std::string& title);
    // Records what created the Thread (Origin Links); `kind` is one of the origin kinds above.
    Status set_origin(const std::string& thread_id, const std::string& kind, const json& ref);

    // ---- Folders (Folders Orthogonal To Smart Views) ----
    Result<Folder> create_folder(const std::string& name);
    Status rename_folder(const std::string& id, const std::string& name);
    // Deletes the folder; its Threads move to the Inbox.
    Status delete_folder(const std::string& id);
    [[nodiscard]] std::vector<Folder> folders();
    // Moves the Thread into the folder, or to the Inbox when `folder_id` is empty.
    Status move(const std::string& thread_id, const std::string& folder_id);

    // ---- Search (Full-Text Search, design D3) ----
    // Matches every term as a prefix, ignoring case and diacritics, across titles and messages of all Threads, archived
    // ones included. The best matches first.
    [[nodiscard]] std::vector<SearchHit> search(const std::string& query, int limit = 50);

    // ---- Drafts (Composer, design D12); `thread_key` is a Thread id or "new" ----
    [[nodiscard]] Draft draft(const std::string& thread_key);
    Status save_draft_text(const std::string& thread_key, const std::string& text);
    Result<DraftAttachment> add_draft_attachment(const std::string& thread_key, const std::string& name, std::string_view content);
    Status remove_draft_attachment(const std::string& thread_key, const std::string& id);
    Result<std::string> draft_attachment_content(const std::string& id);
    Status clear_draft(const std::string& thread_key);
    // Moves the draft of the New Thread composer to the Thread created from it.
    Status move_draft(const std::string& from_key, const std::string& to_key);

    // ---- Fork (Fork From Message, design D10) ----
    // Creates a Thread with origin fork holding the conversation of `thread_id` up to and including the message event
    // (run_id, seq). Returns the new Thread id.
    Result<std::string> fork(const std::string& thread_id, const std::string& run_id, long long seq);

    // ---- Delete (Delete With Confirmation) ----
    // Removes the Thread with its Runs, events, Action items, queued messages, compactions, attachments, draft and
    // search entries. Fails with "thread_busy" while a top-level Run of the Thread is queued or running.
    Status delete_thread(const std::string& thread_id);

    // ---- Action items of a Thread, open and resolved, oldest first (the feed's cards) ----
    [[nodiscard]] std::vector<runtime::ActionItem> actions(const std::string& thread_id);

    // ---- Settings of the inbox (Notifications, Localization), in the settings table ----
    [[nodiscard]] std::optional<std::string> setting(const std::string& key);
    Status set_setting(const std::string& key, const std::string& value);

private:
    std::string now() const { return iso8601_utc(clock_.now()); }
    bool thread_exists(const std::string& id);
    Status update_flag(const std::string& thread_id, const char* column, bool value);

    bool chats_listed() const { return !chats_listed_ || chats_listed_(); }

    Database& db_;
    const Clock& clock_;
    std::function<bool()> chats_listed_;
};

}  // namespace wb::inbox
