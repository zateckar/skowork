#include "inbox/inbox_store.h"

#include "base/strings.h"
#include "platform/crypto.h"

namespace wb::inbox {

namespace {

constexpr const char* kOrigins[] = {"user", "trigger", "remote", "meeting", "fork", "system"};

// The columns of a ThreadEntry; derived flags come from Runs and Action items. The names are those the merged list
// orders and pages by.
constexpr const char* kEntrySelect =
    "SELECT t.id AS id, t.title, t.preview, t.last_activity AS last_activity, t.created_at, t.origin_kind, t.origin_ref, "
    "t.unread, t.archived, t.pinned AS pinned, COALESCE(t.folder_id,''), "
    "(SELECT COUNT(*) FROM action_items a WHERE a.thread_id=t.id AND a.resolved_at=''), "
    "EXISTS(SELECT 1 FROM runs r WHERE r.thread_id=t.id AND r.parent_run_id IS NULL AND r.state IN ('queued','running')), "
    "EXISTS(SELECT 1 FROM runs r WHERE r.thread_id=t.id AND r.parent_run_id IS NULL AND r.state='interrupted'), "
    "COALESCE((SELECT r.state FROM runs r WHERE r.thread_id=t.id AND r.parent_run_id IS NULL ORDER BY r.queue_seq DESC LIMIT 1),''), "
    "'thread', 0, '', '' "
    "FROM threads t";

// The same columns for a Chat's entry (add-chats-in-inbox design D2).
constexpr const char* kChatSelect =
    "SELECT 'chat:'||c.chat_id, c.title, c.preview, c.last_activity, '', 'user', 'null', c.unread>0, c.archived, c.pinned, "
    "COALESCE(c.folder_id,''), 0, 0, 0, '', 'chat', c.unread, c.kind, c.presence "
    "FROM chat_inbox c";

ThreadEntry read_entry(Statement& st) {
    ThreadEntry e;
    e.id = st.col_text(0);
    e.title = st.col_text(1);
    e.preview = st.col_text(2);
    e.last_activity = st.col_text(3);
    e.created_at = st.col_text(4);
    e.origin_kind = st.col_text(5);
    e.origin_ref = json::parse(st.col_text(6), nullptr, false);
    if (e.origin_ref.is_discarded()) e.origin_ref = nullptr;
    e.unread = st.col_int(7) != 0;
    e.archived = st.col_int(8) != 0;
    e.pinned = st.col_int(9) != 0;
    e.folder_id = st.col_text(10);
    e.open_actions = static_cast<int>(st.col_int(11));
    e.running = st.col_int(12) != 0;
    e.has_action = e.open_actions > 0 || st.col_int(13) != 0;  // Action: an open Action item or an interrupted Run
    e.last_run_state = st.col_text(14);
    e.kind = st.col_text(15);
    e.unread_count = st.col_int(16);
    e.chat_kind = st.col_text(17);
    e.presence = st.col_text(18);
    return e;
}

// The WHERE condition of a view; `folder` receives the folder id of "folder:<id>".
std::string view_condition(const std::string& view, std::string& folder) {
    const std::string action =
        "(EXISTS(SELECT 1 FROM action_items a WHERE a.thread_id=t.id AND a.resolved_at='') OR "
        "EXISTS(SELECT 1 FROM runs r WHERE r.thread_id=t.id AND r.parent_run_id IS NULL AND r.state='interrupted'))";
    const std::string running =
        "EXISTS(SELECT 1 FROM runs r WHERE r.thread_id=t.id AND r.parent_run_id IS NULL AND r.state IN ('queued','running'))";
    if (view == "unread") return "t.unread=1 AND t.archived=0";
    if (view == "action") return action;
    if (view == "running") return running;
    if (view == "all") return "t.archived=0";
    if (view == "archive") return "t.archived=1";
    if (view == "inbox") return "t.archived=0 AND t.folder_id IS NULL";
    if (view == "chats") return "0";
    folder = view.substr(7);
    return "t.archived=0 AND t.folder_id=?1";
}

// The WHERE condition of a view for Chat entries: never in Action or Running.
std::string chat_condition(const std::string& view) {
    if (view == "unread") return "c.unread>0 AND c.archived=0";
    if (view == "action" || view == "running") return "0";
    if (view == "all" || view == "chats") return "c.archived=0";
    if (view == "archive") return "c.archived=1";
    if (view == "inbox") return "c.archived=0 AND c.folder_id IS NULL";
    return "c.archived=0 AND c.folder_id=?1";
}

std::string chat_id_of(const std::string& entry_id) { return entry_id.substr(kChatEntryPrefix.size()); }

std::string new_id(const char* prefix) { return std::string(prefix) + random_urlsafe(9); }

// Search terms as FTS5 prefix queries: letters and digits only, each term quoted.
std::string fts_query(const std::string& q) {
    std::string out, term;
    const auto flush = [&] {
        if (term.empty()) return;
        if (!out.empty()) out += ' ';
        out += '"' + term + "\"*";
        term.clear();
    };
    for (const char c : q) {
        const auto u = static_cast<unsigned char>(c);
        if (u >= 0x80 || std::isalnum(u)) term += c;  // UTF-8 bytes of letters stay together
        else flush();
    }
    flush();
    return out;
}

}  // namespace

bool valid_view(const std::string& view) {
    static const char* fixed[] = {"unread", "action", "running", "all", "archive", "inbox", "chats"};
    for (const char* v : fixed)
        if (view == v) return true;
    return starts_with(view, "folder:") && view.size() > 7;
}

bool InboxStore::thread_exists(const std::string& id) {
    auto st = db_.prepare("SELECT 1 FROM threads WHERE id=?1");
    st.bind(1, id);
    return st.step();
}

// ---- Views ------------------------------------------------------------------------------------------------------------

Result<Page> InboxStore::list(const std::string& view, const std::string& cursor, int limit) {
    if (!valid_view(view)) return make_error("invalid_params", "unknown view " + view);
    limit = std::clamp(limit, 1, 500);
    std::string folder;
    // Threads and Chats in one order (pinned, last activity, id), paged by the same key.
    std::string sql = "SELECT * FROM (" + std::string(kEntrySelect) + " WHERE " + view_condition(view, folder);
    if (chats_listed()) sql += " UNION ALL " + std::string(kChatSelect) + " WHERE " + chat_condition(view);
    sql += ") WHERE 1";
    long long c_pinned = 0;
    std::string c_at, c_id;
    const bool has_cursor = !cursor.empty();
    if (has_cursor) {
        const auto p1 = cursor.find('~'), p2 = cursor.rfind('~');
        if (p1 == std::string::npos || p1 == p2) return make_error("invalid_params", "invalid cursor");
        c_pinned = cursor[0] == '1' ? 1 : 0;
        c_at = cursor.substr(p1 + 1, p2 - p1 - 1);
        c_id = cursor.substr(p2 + 1);
        sql += " AND (pinned<?2 OR (pinned=?2 AND (last_activity<?3 OR (last_activity=?3 AND id<?4))))";
    }
    sql += " ORDER BY pinned DESC, last_activity DESC, id DESC LIMIT ?5";
    const std::lock_guard lock(db_.mutex());
    auto st = db_.prepare(sql);
    if (!folder.empty()) st.bind(1, folder);
    if (has_cursor) st.bind(2, c_pinned).bind(3, c_at).bind(4, c_id);
    st.bind(5, static_cast<long long>(limit) + 1);
    Page page;
    while (st.step()) page.entries.push_back(read_entry(st));
    if (!st.status()) return st.status().error();
    if (static_cast<int>(page.entries.size()) > limit) {
        page.entries.pop_back();
        const ThreadEntry& last = page.entries.back();
        page.next_cursor = std::string(last.pinned ? "1" : "0") + "~" + last.last_activity + "~" + last.id;
    }
    return page;
}

std::optional<ThreadEntry> InboxStore::entry(const std::string& thread_id) {
    const bool chat = is_chat_entry(thread_id);
    if (chat && !chats_listed()) return std::nullopt;
    const std::lock_guard lock(db_.mutex());
    auto st = db_.prepare(chat ? std::string(kChatSelect) + " WHERE c.chat_id=?1" : std::string(kEntrySelect) + " WHERE t.id=?1");
    st.bind(1, chat ? chat_id_of(thread_id) : thread_id);
    if (!st.step()) return std::nullopt;
    return read_entry(st);
}

Counts InboxStore::counts() {
    const std::lock_guard lock(db_.mutex());
    Counts c;
    std::string unused;
    for (const auto& [view, out] : {std::pair<const char*, int*>{"unread", &c.unread}, {"action", &c.action}, {"running", &c.running}}) {
        auto st = db_.prepare("SELECT COUNT(*) FROM threads t WHERE " + view_condition(view, unused));
        if (st.step()) *out = static_cast<int>(st.col_int(0));
    }
    // Unread counts Chats with unread messages too (Smart Views).
    if (chats_listed()) {
        auto st = db_.prepare("SELECT COUNT(*) FROM chat_inbox c WHERE " + chat_condition("unread"));
        if (st.step()) c.unread += static_cast<int>(st.col_int(0));
    }
    return c;
}

// ---- Triage ------------------------------------------------------------------------------------------------------------

Status InboxStore::update_flag(const std::string& thread_id, const char* column, bool value) {
    const bool chat = is_chat_entry(thread_id);
    // A Chat is read when it is opened; its read state is not set by hand.
    if (chat && std::string_view(column) == "unread") return make_error("invalid_params", "a Chat is read when it is opened");
    const std::lock_guard lock(db_.mutex());
    auto st = db_.prepare(chat ? std::string("UPDATE chat_inbox SET ") + column + "=?2 WHERE chat_id=?1"
                               : std::string("UPDATE threads SET ") + column + "=?2 WHERE id=?1");
    st.bind(1, chat ? chat_id_of(thread_id) : thread_id).bind(2, value ? 1 : 0);
    if (auto s = st.run(); !s) return s;
    if (db_.changes() == 0) return make_error("not_found", chat ? "no such Chat" : "no such Thread");
    return {};
}

Status InboxStore::set_unread(const std::string& thread_id, bool unread) { return update_flag(thread_id, "unread", unread); }
Status InboxStore::set_archived(const std::string& thread_id, bool archived) { return update_flag(thread_id, "archived", archived); }
Status InboxStore::set_pinned(const std::string& thread_id, bool pinned) { return update_flag(thread_id, "pinned", pinned); }

Status InboxStore::rename(const std::string& thread_id, const std::string& title) {
    if (is_chat_entry(thread_id)) return make_error("invalid_params", "a Chat is renamed in its conversation");
    std::string t = trim(title);
    if (t.empty()) return make_error("invalid_params", "the title must not be empty");
    if (t.size() > 200) t = utf8_truncate(t, 200);
    const std::lock_guard lock(db_.mutex());
    auto st = db_.prepare("UPDATE threads SET title=?2 WHERE id=?1");
    st.bind(1, thread_id).bind(2, t);
    if (auto s = st.run(); !s) return s;
    if (db_.changes() == 0) return make_error("not_found", "no such Thread");
    return {};
}

Status InboxStore::set_origin(const std::string& thread_id, const std::string& kind, const json& ref) {
    if (std::find(std::begin(kOrigins), std::end(kOrigins), kind) == std::end(kOrigins))
        return make_error("invalid_params", "unknown origin " + kind);
    const std::lock_guard lock(db_.mutex());
    auto st = db_.prepare("UPDATE threads SET origin_kind=?2, origin_ref=?3 WHERE id=?1");
    st.bind(1, thread_id).bind(2, kind).bind(3, ref.dump());
    if (auto s = st.run(); !s) return s;
    if (db_.changes() == 0) return make_error("not_found", "no such Thread");
    return {};
}

// ---- Folders -------------------------------------------------------------------------------------------------------------

Result<Folder> InboxStore::create_folder(const std::string& name) {
    const std::string n = trim(name);
    if (n.empty() || n.size() > 100) return make_error("invalid_params", "a folder name has 1 to 100 characters");
    Folder f{new_id("fld_"), n, now()};
    const std::lock_guard lock(db_.mutex());
    auto st = db_.prepare("INSERT INTO folders(id,name,created_at) VALUES(?1,?2,?3)");
    st.bind(1, f.id).bind(2, f.name).bind(3, f.created_at);
    if (auto s = st.run(); !s) return s.error();
    return f;
}

Status InboxStore::rename_folder(const std::string& id, const std::string& name) {
    const std::string n = trim(name);
    if (n.empty() || n.size() > 100) return make_error("invalid_params", "a folder name has 1 to 100 characters");
    const std::lock_guard lock(db_.mutex());
    auto st = db_.prepare("UPDATE folders SET name=?2 WHERE id=?1");
    st.bind(1, id).bind(2, n);
    if (auto s = st.run(); !s) return s;
    if (db_.changes() == 0) return make_error("not_found", "no such folder");
    return {};
}

Status InboxStore::delete_folder(const std::string& id) {
    const std::lock_guard lock(db_.mutex());
    return db_.transaction([&]() -> Status {
        for (const char* sql : {"UPDATE threads SET folder_id=NULL WHERE folder_id=?1", "UPDATE chat_inbox SET folder_id=NULL WHERE folder_id=?1"}) {
            auto mv = db_.prepare(sql);
            mv.bind(1, id);
            if (auto s = mv.run(); !s) return s;
        }
        auto del = db_.prepare("DELETE FROM folders WHERE id=?1");
        del.bind(1, id);
        if (auto s = del.run(); !s) return s;
        if (db_.changes() == 0) return make_error("not_found", "no such folder");
        return {};
    });
}

std::vector<Folder> InboxStore::folders() {
    const std::lock_guard lock(db_.mutex());
    std::vector<Folder> out;
    auto st = db_.prepare("SELECT id,name,created_at FROM folders ORDER BY name COLLATE NOCASE, id");
    while (st.step()) out.push_back({st.col_text(0), st.col_text(1), st.col_text(2)});
    return out;
}

Status InboxStore::move(const std::string& thread_id, const std::string& folder_id) {
    const std::lock_guard lock(db_.mutex());
    if (!folder_id.empty()) {
        auto f = db_.prepare("SELECT 1 FROM folders WHERE id=?1");
        f.bind(1, folder_id);
        if (!f.step()) return make_error("not_found", "no such folder");
    }
    const bool chat = is_chat_entry(thread_id);
    auto st = db_.prepare(chat ? "UPDATE chat_inbox SET folder_id=?2 WHERE chat_id=?1" : "UPDATE threads SET folder_id=?2 WHERE id=?1");
    st.bind(1, chat ? chat_id_of(thread_id) : thread_id);
    if (folder_id.empty()) st.bind_null(2);
    else st.bind(2, folder_id);
    if (auto s = st.run(); !s) return s;
    if (db_.changes() == 0) return make_error("not_found", chat ? "no such Chat" : "no such Thread");
    return {};
}

// ---- Search ---------------------------------------------------------------------------------------------------------------

std::vector<SearchHit> InboxStore::search(const std::string& query, int limit) {
    std::vector<SearchHit> out;
    const std::string q = fts_query(query);
    if (q.empty()) return out;
    const std::lock_guard lock(db_.mutex());
    auto st = db_.prepare(
        "SELECT s.thread_id, t.title, s.run_id, s.seq, snippet(thread_search, -1, '[[', ']]', '...', 16), t.archived "
        "FROM thread_search s JOIN threads t ON t.id=s.thread_id WHERE thread_search MATCH ?1 ORDER BY rank LIMIT ?2");
    st.bind(1, q).bind(2, static_cast<long long>(std::clamp(limit, 1, 200)));
    while (st.step()) {
        SearchHit h;
        h.thread_id = st.col_text(0);
        h.title = st.col_text(1);
        h.run_id = st.col_text(2);
        h.seq = st.col_int(3);
        h.snippet = st.col_text(4);
        h.archived = st.col_int(5) != 0;
        out.push_back(std::move(h));
    }
    return out;
}

// ---- Drafts -----------------------------------------------------------------------------------------------------------------

Draft InboxStore::draft(const std::string& key) {
    const std::lock_guard lock(db_.mutex());
    Draft d;
    d.thread_key = key;
    auto st = db_.prepare("SELECT text,updated_at FROM drafts WHERE thread_key=?1");
    st.bind(1, key);
    if (st.step()) {
        d.text = st.col_text(0);
        d.updated_at = st.col_text(1);
    }
    auto at = db_.prepare("SELECT id,name,size FROM draft_attachments WHERE thread_key=?1 ORDER BY created_at, id");
    at.bind(1, key);
    while (at.step()) d.attachments.push_back({at.col_text(0), at.col_text(1), at.col_int(2)});
    return d;
}

Status InboxStore::save_draft_text(const std::string& key, const std::string& text) {
    const std::lock_guard lock(db_.mutex());
    auto st = db_.prepare(
        "INSERT INTO drafts(thread_key,text,updated_at) VALUES(?1,?2,?3) "
        "ON CONFLICT(thread_key) DO UPDATE SET text=excluded.text, updated_at=excluded.updated_at");
    st.bind(1, key).bind(2, text).bind(3, now());
    return st.run();
}

Result<DraftAttachment> InboxStore::add_draft_attachment(const std::string& key, const std::string& name, std::string_view content) {
    const std::string n = trim(name);
    if (n.empty()) return make_error("invalid_params", "the attachment needs a name");
    if (static_cast<long long>(content.size()) > kMaxAttachmentBytes)
        return make_error("too_large", "an attachment can be at most 25 MB");
    const std::lock_guard lock(db_.mutex());
    {
        auto c = db_.prepare("SELECT COUNT(*) FROM draft_attachments WHERE thread_key=?1");
        c.bind(1, key);
        if (c.step() && c.col_int(0) >= kMaxAttachments) return make_error("too_many", "a message can have at most 10 attachments");
    }
    DraftAttachment a{new_id("dat_"), n, static_cast<long long>(content.size())};
    auto st = db_.prepare("INSERT INTO draft_attachments(id,thread_key,name,size,content,created_at) VALUES(?1,?2,?3,?4,?5,?6)");
    st.bind(1, a.id).bind(2, key).bind(3, a.name).bind(4, a.size);
    st.bind_blob(5, std::vector<std::uint8_t>(content.begin(), content.end())).bind(6, now());
    if (auto s = st.run(); !s) return s.error();
    return a;
}

Status InboxStore::remove_draft_attachment(const std::string& key, const std::string& id) {
    const std::lock_guard lock(db_.mutex());
    auto st = db_.prepare("DELETE FROM draft_attachments WHERE id=?1 AND thread_key=?2");
    st.bind(1, id).bind(2, key);
    if (auto s = st.run(); !s) return s;
    if (db_.changes() == 0) return make_error("not_found", "no such attachment");
    return {};
}

Result<std::string> InboxStore::draft_attachment_content(const std::string& id) {
    const std::lock_guard lock(db_.mutex());
    auto st = db_.prepare("SELECT content FROM draft_attachments WHERE id=?1");
    st.bind(1, id);
    if (!st.step()) return make_error("not_found", "no such attachment");
    const auto b = st.col_blob(0);
    return std::string(b.begin(), b.end());
}

Status InboxStore::clear_draft(const std::string& key) {
    const std::lock_guard lock(db_.mutex());
    return db_.transaction([&]() -> Status {
        auto a = db_.prepare("DELETE FROM draft_attachments WHERE thread_key=?1");
        a.bind(1, key);
        if (auto s = a.run(); !s) return s;
        auto d = db_.prepare("DELETE FROM drafts WHERE thread_key=?1");
        d.bind(1, key);
        return d.run();
    });
}

Status InboxStore::move_draft(const std::string& from, const std::string& to) {
    const std::lock_guard lock(db_.mutex());
    return db_.transaction([&]() -> Status {
        auto a = db_.prepare("UPDATE draft_attachments SET thread_key=?2 WHERE thread_key=?1");
        a.bind(1, from).bind(2, to);
        if (auto s = a.run(); !s) return s;
        auto del = db_.prepare("DELETE FROM drafts WHERE thread_key=?1");
        del.bind(1, to);
        if (auto s = del.run(); !s) return s;
        auto d = db_.prepare("UPDATE drafts SET thread_key=?2 WHERE thread_key=?1");
        d.bind(1, from).bind(2, to);
        return d.run();
    });
}

// ---- Fork -------------------------------------------------------------------------------------------------------------------

Result<std::string> InboxStore::fork(const std::string& thread_id, const std::string& run_id, long long seq) {
    const std::lock_guard lock(db_.mutex());
    std::string title;
    {
        auto t = db_.prepare("SELECT title FROM threads WHERE id=?1");
        t.bind(1, thread_id);
        if (!t.step()) return make_error("not_found", "no such Thread");
        title = t.col_text(0);
    }
    {
        auto m = db_.prepare(
            "SELECT 1 FROM run_events e JOIN runs r ON r.id=e.run_id WHERE r.thread_id=?1 AND r.parent_run_id IS NULL "
            "AND e.run_id=?2 AND e.seq=?3 AND e.kind='message'");
        m.bind(1, thread_id).bind(2, run_id).bind(3, seq);
        if (!m.step()) return make_error("not_found", "no such message in the Thread");
    }
    // The messages up to and including the chosen one, in conversation order.
    std::vector<std::pair<std::string, std::string>> messages;  // (at, payload)
    {
        auto e = db_.prepare(
            "SELECT e.run_id, e.seq, e.at, e.payload FROM run_events e JOIN runs r ON r.id=e.run_id "
            "WHERE r.thread_id=?1 AND r.parent_run_id IS NULL AND e.kind='message' ORDER BY r.queue_seq, e.seq");
        e.bind(1, thread_id);
        while (e.step()) {
            messages.emplace_back(e.col_text(2), e.col_text(3));
            if (e.col_text(0) == run_id && e.col_int(1) == seq) break;
        }
    }
    const std::string new_thread = new_id("thr_");
    const std::string fork_run = new_id("run_");
    const std::string at = now();
    const json ref = {{"thread_id", thread_id}, {"run_id", run_id}, {"seq", seq}, {"title", title}};
    const auto s = db_.transaction([&]() -> Status {
        auto t = db_.prepare(
            "INSERT INTO threads(id,title,created_at,updated_at,origin_kind,origin_ref,last_activity) VALUES(?1,?2,?3,?3,'fork',?4,?3)");
        t.bind(1, new_thread).bind(2, title).bind(3, at).bind(4, ref.dump());
        if (auto r = t.run(); !r) return r;
        long long qseq = 1;
        {
            auto q = db_.prepare("SELECT COALESCE(MAX(queue_seq),0)+1 FROM runs");
            if (q.step()) qseq = q.col_int(0);
        }
        auto r = db_.prepare(
            "INSERT INTO runs(id,thread_id,parent_run_id,state,reason,origin_kind,origin_ref,queue_seq,role,created_at,started_at,ended_at) "
            "VALUES(?1,?2,NULL,'succeeded','forked','user_message',?3,?4,'fork',?5,?5,?5)");
        r.bind(1, fork_run).bind(2, new_thread).bind(3, ref.dump()).bind(4, qseq).bind(5, at);
        if (auto x = r.run(); !x) return x;
        long long n = 0;
        auto ev = db_.prepare("INSERT INTO run_events(run_id,seq,at,kind,payload) VALUES(?1,?2,?3,?4,?5)");
        ev.bind(1, fork_run).bind(2, ++n).bind(3, at).bind(4, "state").bind(5, json{{"state", "succeeded"}, {"reason", "forked"}}.dump());
        if (auto x = ev.run(); !x) return x;
        for (const auto& [m_at, payload] : messages) {
            auto m = db_.prepare("INSERT INTO run_events(run_id,seq,at,kind,payload) VALUES(?1,?2,?3,'message',?4)");
            m.bind(1, fork_run).bind(2, ++n).bind(3, m_at).bind(4, payload);
            if (auto x = m.run(); !x) return x;
        }
        return {};
    });
    if (!s) return s.error();
    return new_thread;
}

// ---- Delete -------------------------------------------------------------------------------------------------------------------

Status InboxStore::delete_thread(const std::string& thread_id) {
    const std::lock_guard lock(db_.mutex());
    if (!thread_exists(thread_id)) return make_error("not_found", "no such Thread");
    {
        auto busy = db_.prepare("SELECT 1 FROM runs WHERE thread_id=?1 AND state IN ('queued','running','interrupted')");
        busy.bind(1, thread_id);
        if (busy.step()) return make_error("thread_busy", "a Run of this Thread is still active; stop it first");
    }
    return db_.transaction([&]() -> Status {
        const char* statements[] = {
            "DELETE FROM run_events WHERE run_id IN (SELECT id FROM runs WHERE thread_id=?1)",
            "DELETE FROM action_items WHERE thread_id=?1",
            "DELETE FROM queued_messages WHERE thread_id=?1",
            "DELETE FROM thread_compactions WHERE thread_id=?1",
            "DELETE FROM thread_attachments WHERE thread_id=?1",
            "DELETE FROM draft_attachments WHERE thread_key=?1",
            "DELETE FROM drafts WHERE thread_key=?1",
            "DELETE FROM runs WHERE thread_id=?1",
            "DELETE FROM threads WHERE id=?1",  // the trigger removes the search entries
        };
        for (const char* sql : statements) {
            auto st = db_.prepare(sql);
            st.bind(1, thread_id);
            if (auto s = st.run(); !s) return s;
        }
        return {};
    });
}

// ---- Action items -------------------------------------------------------------------------------------------------------------

std::vector<runtime::ActionItem> InboxStore::actions(const std::string& thread_id) {
    const std::lock_guard lock(db_.mutex());
    std::vector<runtime::ActionItem> out;
    auto st = db_.prepare(
        "SELECT id,thread_id,COALESCE(run_id,''),kind,call_id,payload,created_at,resolved_at,resolution,raised_by "
        "FROM action_items WHERE thread_id=?1 ORDER BY created_at, id");
    st.bind(1, thread_id);
    while (st.step()) {
        runtime::ActionItem a;
        a.id = st.col_text(0);
        a.thread_id = st.col_text(1);
        a.run_id = st.col_text(2);
        a.kind = st.col_text(3);
        a.call_id = st.col_text(4);
        a.payload = json::parse(st.col_text(5), nullptr, false);
        if (a.payload.is_discarded()) a.payload = nullptr;
        a.created_at = st.col_text(6);
        a.resolved_at = st.col_text(7);
        a.resolution = json::parse(st.col_text(8), nullptr, false);
        if (a.resolution.is_discarded()) a.resolution = nullptr;
        a.raised_by = st.col_text(9);
        out.push_back(std::move(a));
    }
    return out;
}

// ---- Settings -----------------------------------------------------------------------------------------------------------------

std::optional<std::string> InboxStore::setting(const std::string& key) {
    const std::lock_guard lock(db_.mutex());
    auto st = db_.prepare("SELECT value FROM settings WHERE key=?1");
    st.bind(1, key);
    if (!st.step()) return std::nullopt;
    return st.col_text(0);
}

Status InboxStore::set_setting(const std::string& key, const std::string& value) {
    const std::lock_guard lock(db_.mutex());
    auto st = db_.prepare(
        "INSERT INTO settings(key,value,updated_at) VALUES(?1,?2,?3) "
        "ON CONFLICT(key) DO UPDATE SET value=excluded.value, updated_at=excluded.updated_at");
    st.bind(1, key).bind(2, value).bind(3, now());
    return st.run();
}

}  // namespace wb::inbox
