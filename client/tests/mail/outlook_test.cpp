// client/mail-calendar on the Local Outlook backend (add-local-outlook-and-teams tasks 2.1, 2.2): backend selection,
// and the real MailService, MailSync, MailStore and tools over OutlookGraph with a fake OutlookSession, since the
// development machine has no Outlook (the COM session is validated by spike 1.1).
#include "support/graph_mock.h"

#include "mail/mail_service.h"
#include "mail/outlook_graph.h"
#include "base/time.h"
#include "base/url.h"

#include <algorithm>
#include <map>
#include <set>

using namespace wb;
using namespace wb::mail;
using namespace wb::mail::outlook;

namespace {

constexpr long long kHour = 3'600'000, kDay = 86'400'000;
constexpr const char* kSecretLabel = "11111111-2222-3333-4444-555555555555";

// An in-memory Outlook: folders, messages and events, with the writes the tools make recorded.
class FakeOutlook final : public OutlookSession {
public:
    FakeOutlook() {
        folders_ = {{"F-INBOX", "Inbox", "inbox"}, {"F-SENT", "Sent Items", "sentitems"}, {"F-DRAFTS", "Drafts", "drafts"},
                    {"F-DEL", "Deleted Items", "deleteditems"}, {"F-PRIV", "Private", ""}};
    }

    OlMessage& add(const std::string& id, long long received, const std::string& subject, const std::string& folder = "F-INBOX") {
        OlMessage m;
        m.id = id;
        m.folder_id = folder;
        m.conversation_id = "C-" + id;
        m.received = received;
        m.modified = received;
        m.subject = subject;
        m.body = "Body of " + subject;
        m.from = {"Petra Malá", "petra@example.com"};
        m.recipients = {{{"Jan Novák", "jan@example.com"}, "to", ""}};
        m.is_read = false;
        messages_[id] = m;
        return messages_[id];
    }
    OlEvent& add_event(const std::string& id, long long start, long long end, const std::string& subject) {
        OlEvent e;
        e.id = id;
        e.start = start;
        e.end = end;
        e.modified = start - kDay;
        e.subject = subject;
        e.organizer = {"Petra Malá", "petra@example.com"};
        e.is_meeting = true;
        e.attendees = {{{"Jan Novák", "jan@example.com"}, "required", "accepted"}};
        e.response = "accepted";
        e.show_as = "busy";
        e.sensitivity = "normal";
        events_[id] = e;
        return events_[id];
    }

    Result<OlAddress> me() override { return OlAddress{"Jan Novák", "jan@example.com"}; }
    Result<std::vector<OlFolder>> folders() override { return folders_; }
    Result<std::vector<OlRow>> rows(const std::string& folder_id, long long since) override {
        if (down) return make_error("offline", "Outlook is not running");
        ++row_reads;
        std::vector<OlRow> out;
        for (const auto& [id, m] : messages_)
            if (m.folder_id == folder_id && m.received >= since) out.push_back({id, m.modified, m.received});
        return out;
    }
    Result<OlMessage> message(const std::string& id) override {
        if (down) return make_error("offline", "Outlook is not running");
        ++opened;
        if (unreadable_.count(id)) return make_error("bad_request", "Your Digital ID name cannot be found by the underlying security system.");
        const auto it = messages_.find(id);
        if (it == messages_.end()) return make_error("not_found", "no such item");
        return it->second;
    }
    Result<std::vector<OlMessage>> find(const MessageQuery& q) override {
        if (down) return make_error("offline", "Outlook is not running");
        last_query = q;
        std::vector<OlMessage> out;
        for (const auto& [id, m] : messages_) {
            if (!q.folder_id.empty() && m.folder_id != q.folder_id) continue;
            if (!q.conversation_id.empty() && m.conversation_id != q.conversation_id) continue;
            if (!q.text.empty() && m.subject.find(q.text) == std::string::npos && m.body.find(q.text) == std::string::npos) continue;
            if (q.since && m.received < q.since) continue;
            out.push_back(m);
        }
        return out;
    }
    Result<std::vector<OlAttachment>> attachments(const std::string&) override { return std::vector<OlAttachment>{{"1", "notes.txt", "text/plain", 5}}; }
    Result<std::string> attachment_bytes(const std::string&, const std::string&) override { return std::string("hello"); }
    Result<OlMessage> create_reply(const std::string& id, const std::string& kind, const std::string& comment) override {
        writes.push_back("create_reply:" + id + ":" + kind);
        auto& d = add("D" + std::to_string(++drafts_), messages_[id].received + 1, "RE: " + messages_[id].subject, "F-DRAFTS");
        d.body = comment;
        d.is_draft = true;
        return d;
    }
    Result<OlMessage> create_draft(const DraftPatch& p) override {
        writes.push_back("create_draft");
        auto& d = add("D" + std::to_string(++drafts_), now_ms, p.subject.value_or(""), "F-DRAFTS");
        d.is_draft = true;
        return update_draft(d.id, p);
    }
    Result<OlMessage> update_draft(const std::string& id, const DraftPatch& p) override {
        writes.push_back("update_draft:" + id);
        auto& d = messages_[id];
        if (p.subject) d.subject = *p.subject;
        if (p.body) d.body = *p.body;
        if (p.to) {
            std::erase_if(d.recipients, [](const OlRecipient& r) { return r.type == "to"; });
            for (const auto& a : *p.to) d.recipients.push_back({{"", a}, "to", ""});
        }
        ++d.modified;
        return d;
    }
    Status send(const std::string& id) override {
        sent.push_back(messages_[id]);
        messages_.erase(id);
        return {};
    }
    Status send_new(const DraftPatch& p) override {
        OlMessage m;
        m.subject = p.subject.value_or("");
        m.body = p.body.value_or("");
        for (const auto& a : p.to.value_or(std::vector<std::string>{})) m.recipients.push_back({{"", a}, "to", ""});
        sent.push_back(m);
        return {};
    }
    Result<OlMessage> move(const std::string& id, const std::string& folder) override {
        messages_[id].folder_id = folder;
        return messages_[id];
    }
    Status remove(const std::string& id) override {
        messages_[id].folder_id = "F-DEL";
        return {};
    }
    Result<std::vector<OlEvent>> events(long long from, long long to, size_t) override {
        if (down) return make_error("offline", "Outlook is not running");
        ++event_reads;
        std::vector<OlEvent> out;
        for (const auto& [id, e] : events_)
            if (e.end > from && e.start < to) out.push_back(e);
        return out;
    }
    Result<std::string> folder_stamp(const std::string& folder_id) override {
        if (down) return make_error("offline", "Outlook is not running");
        if (!stamps) return std::string();
        long long n = 0, newest = 0;
        for (const auto& [id, m] : messages_)
            if (m.folder_id == folder_id) {
                ++n;
                newest = std::max(newest, m.modified);
            }
        return std::to_string(n) + "/" + std::to_string(newest);
    }
    Result<std::string> calendar_stamp() override {
        if (down) return make_error("offline", "Outlook is not running");
        if (!stamps) return std::string();
        long long newest = 0;
        for (const auto& [id, e] : events_) newest = std::max(newest, e.modified);
        return std::to_string(events_.size()) + "/" + std::to_string(newest);
    }
    // Calendar items: series (by series_id) and single events, with the newest modification of each.
    Result<std::vector<OlRow>> calendar_items() override {
        if (down) return make_error("offline", "Outlook is not running");
        if (!item_table) return std::vector<OlRow>{};
        std::map<std::string, long long> newest;
        for (const auto& [id, e] : events_) {
            auto& n = newest[e.series_id.empty() ? id : e.series_id];
            n = std::max(n, e.modified);
        }
        std::vector<OlRow> out;
        for (const auto& [id, m] : newest) out.push_back({id, m, 0});
        return out;
    }
    Result<std::vector<OlEvent>> item_events(const std::string& entry, long long from, long long to) override {
        if (down) return make_error("offline", "Outlook is not running");
        items_read.push_back(entry);
        std::vector<OlEvent> out;
        bool exists = false;
        for (const auto& [id, e] : events_)
            if ((e.series_id.empty() ? id : e.series_id) == entry) {
                exists = true;
                if (e.end > from && e.start < to) out.push_back(e);
            }
        if (!exists) return make_error("not_found", "no such item");
        return out;
    }
    Result<OlEvent> event(const std::string& id) override {
        const auto it = events_.find(id);
        if (it == events_.end()) return make_error("not_found", "no such event");
        return it->second;
    }
    Result<OlEvent> save_event(const std::string& id, const EventPatch& p) override {
        ++saved_events;
        const std::string eid = id.empty() ? "E-NEW" + std::to_string(saved_events) : id;
        auto& e = events_[eid];
        e.id = eid;
        if (p.subject) e.subject = *p.subject;
        if (p.start) e.start = *p.start;
        if (p.end) e.end = *p.end;
        if (p.required) {
            e.attendees.clear();
            for (const auto& a : *p.required) e.attendees.push_back({{"", a}, "required", "none"});
        }
        e.modified = now_ms;
        return e;
    }
    Status respond(const std::string& id, const std::string& response, const std::string&) override {
        writes.push_back("respond:" + id + ":" + response);
        return {};
    }
    Result<std::string> free_busy(const std::string& address, long long, int, int slots) override {
        if (address == "nobody@example.com") return make_error("not_found", "unresolved");
        return std::string(static_cast<size_t>(slots), '0');
    }
    Status display(const std::string& id) override {
        writes.push_back("display:" + id);
        return {};
    }

    std::vector<OlFolder> folders_;
    std::map<std::string, OlMessage> messages_;
    std::map<std::string, OlEvent> events_;
    std::vector<std::string> writes;
    std::vector<OlMessage> sent;
    std::optional<MessageQuery> last_query;
    std::set<std::string> unreadable_;
    int opened = 0, saved_events = 0, drafts_ = 0, event_reads = 0, row_reads = 0;
    bool stamps = true, item_table = true;
    std::vector<std::string> items_read;
    long long now_ms = 0;
    bool down = false;
};

struct OutlookFixture {
    explicit OutlookFixture(json extra = json::object()) {
        now = unix_millis_now();
        fake = std::make_shared<FakeOutlook>();
        fake->now_ms = now;
        policy = extra;
        make();
    }
    ~OutlookFixture() { svc.reset(); }

    void make() {
        svc.reset();
        MailEnv e;
        e.store = db.store.get();
        e.clock = &system_clock();
        e.launcher = &browser;
        e.policy = policy_from(&policy);
        e.dev_build = true;
        e.background_sync = false;
        e.outlook_installed = [this] { return installed; };
        // The service owns its session; the fake outlives it so a restart sees the same Outlook.
        e.outlook_factory = [this](std::function<bool()>) -> std::unique_ptr<OutlookSession> {
            struct Proxy final : OutlookSession {
                std::shared_ptr<FakeOutlook> f;
                Result<OlAddress> me() override { return f->me(); }
                Result<std::vector<OlFolder>> folders() override { return f->folders(); }
                Result<std::vector<OlRow>> rows(const std::string& a, long long b) override { return f->rows(a, b); }
                Result<OlMessage> message(const std::string& a) override { return f->message(a); }
                Result<std::vector<OlMessage>> find(const MessageQuery& q) override { return f->find(q); }
                Result<std::vector<OlAttachment>> attachments(const std::string& a) override { return f->attachments(a); }
                Result<std::string> attachment_bytes(const std::string& a, const std::string& b) override { return f->attachment_bytes(a, b); }
                Result<OlMessage> create_reply(const std::string& a, const std::string& b, const std::string& c) override { return f->create_reply(a, b, c); }
                Result<OlMessage> create_draft(const DraftPatch& p) override { return f->create_draft(p); }
                Result<OlMessage> update_draft(const std::string& a, const DraftPatch& p) override { return f->update_draft(a, p); }
                Status send(const std::string& a) override { return f->send(a); }
                Status send_new(const DraftPatch& p) override { return f->send_new(p); }
                Result<OlMessage> move(const std::string& a, const std::string& b) override { return f->move(a, b); }
                Status remove(const std::string& a) override { return f->remove(a); }
                Result<std::vector<OlEvent>> events(long long a, long long b, size_t c) override { return f->events(a, b, c); }
                Result<std::string> calendar_stamp() override { return f->calendar_stamp(); }
                Result<std::vector<OlRow>> calendar_items() override { return f->calendar_items(); }
                Result<std::vector<OlEvent>> item_events(const std::string& a, long long b, long long c) override { return f->item_events(a, b, c); }
                Result<std::string> folder_stamp(const std::string& a) override { return f->folder_stamp(a); }
                Result<OlEvent> event(const std::string& a) override { return f->event(a); }
                Result<OlEvent> save_event(const std::string& a, const EventPatch& p) override { return f->save_event(a, p); }
                Status respond(const std::string& a, const std::string& b, const std::string& c) override { return f->respond(a, b, c); }
                Result<std::string> free_busy(const std::string& a, long long b, int c, int d) override { return f->free_busy(a, b, c, d); }
                Status display(const std::string& a) override { return f->display(a); }
            };
            auto p = std::make_unique<Proxy>();
            p->f = fake;
            return p;
        };
        svc = std::make_unique<MailService>(std::move(e));
        svc->on_event([this](const MailEvent& ev) { events.push_back(ev); });
        svc->start();
        tools.clear();
        for (auto& t : svc->tools()) tools[t.id] = std::move(t);
    }
    void restart() {
        svc.reset();
        db.open();
        make();
    }
    json run(const std::string& tool, const json& args, bool* ok = nullptr, const std::string& call_id = {}) {
        runtime::ToolContext ctx;
        ctx.call_id = call_id.empty() ? "call" + std::to_string(++calls) : call_id;
        auto out = tools.at(tool).run(ctx, args);
        if (ok) *ok = out.ok;
        auto j = parse_json(out.content);
        return j ? *j : json(out.content);
    }
    std::vector<MailEvent> take() { return std::exchange(events, {}); }
    int count(const std::vector<MailEvent>& v, const std::string& kind, bool backfill = false) {
        return static_cast<int>(std::count_if(v.begin(), v.end(), [&](const MailEvent& e) {
            return e.kind == kind && jbool(e.detail, "backfill", false) == backfill;
        }));
    }

    long long now = 0;
    bool installed = true;
    std::shared_ptr<FakeOutlook> fake;
    MailDb db;
    FollowingBrowser browser;
    json policy;
    std::unique_ptr<MailService> svc;
    std::map<std::string, runtime::ToolDef> tools;
    std::vector<MailEvent> events;
    int calls = 0;
};

}  // namespace

// ---- Mail Backend Selection ----

TEST(OutlookBackend, AutoChoosesOutlookWithoutAppRegistration) {
    json p = json::object();
    const auto cfg = read_config(policy_from(&p), {}, false, [] { return true; });
    EXPECT_TRUE(cfg.available);
    EXPECT_EQ(cfg.backend, "outlook");
    EXPECT_TRUE(cfg.scopes.empty());
}

TEST(OutlookBackend, AutoChoosesGraphWhenConfigured) {
    json p = {{"mail.oauth.authority", "https://login.microsoftonline.com"}, {"mail.oauth.tenant_id", "t"}, {"mail.oauth.client_id", "c"}};
    const auto cfg = read_config(policy_from(&p), {}, false, [] { return true; });
    EXPECT_TRUE(cfg.available);
    EXPECT_EQ(cfg.backend, "graph");
}

TEST(OutlookBackend, NeitherBackendIsUnavailable) {
    json p = json::object();
    auto cfg = read_config(policy_from(&p), {}, false, [] { return false; });
    EXPECT_FALSE(cfg.available);
    EXPECT_EQ(cfg.unavailable_code, "not_configured");
    p["mail.backend"] = "outlook";
    cfg = read_config(policy_from(&p), {}, false, [] { return false; });
    EXPECT_EQ(cfg.unavailable_code, "outlook_missing");
    p["mail.backend"] = "exchange";
    cfg = read_config(policy_from(&p), {}, false, [] { return true; });
    EXPECT_EQ(cfg.unavailable_code, "policy_error");
}

// ---- the adapter on its own ----

TEST(OutlookGraph, OccurrenceIds) {
    EXPECT_EQ(occurrence_id("ABC", "20261123T0930"), "ABC.20261123T0930");
    const auto s = split_occurrence_id("ABC.20261123T0930");
    ASSERT_TRUE(s);
    EXPECT_EQ(s->first, "ABC");
    EXPECT_EQ(s->second, "20261123T0930");
    EXPECT_FALSE(split_occurrence_id("00000000ABCDEF"));
}

TEST(OutlookGraph, UnknownPathsAndMalformedLinksAreErrorsNotCrashes) {
    FakeOutlook f;
    OutlookGraph g(f, system_clock());
    EXPECT_EQ(g.handle("GET", "/users/x@example.com", std::nullopt).error().code, "bad_request");
    EXPECT_EQ(g.handle("GET", "/me/messages/X", std::nullopt).error().code, "not_found");
    EXPECT_FALSE(g.handle("GET", "outlook-delta|nope|folder|F|x", std::nullopt));
    EXPECT_EQ(g.handle("GET", "outlook-page|nope|1", std::nullopt).error().code, "sync_state_not_found");
    EXPECT_FALSE(g.handle("POST", "/me/messages/X", std::nullopt));
    EXPECT_FALSE(g.handle("GET", "/me/messages?$filter=bogus", std::nullopt));
}

TEST(OutlookGraph, DeltaPagesAndReportsRemovals) {
    FakeOutlook f;
    const long long now = unix_millis_now();
    for (int i = 0; i < 7; ++i) f.add("M" + std::to_string(i), now - i * kHour, "Subject " + std::to_string(i));
    OutlookGraph g(f, system_clock());
    auto first = g.handle("GET", "/me/mailFolders/F-INBOX/messages/delta", std::nullopt, 3);
    ASSERT_TRUE(first);
    EXPECT_EQ((*first)["value"].size(), 3u);
    std::string link = jstr(*first, "@odata.nextLink");
    size_t total = 3;
    std::string delta;
    while (!link.empty()) {
        auto p = g.handle("GET", link, std::nullopt, 3);
        ASSERT_TRUE(p);
        total += (*p)["value"].size();
        link = jstr(*p, "@odata.nextLink");
        delta = jstr(*p, "@odata.deltaLink");
    }
    EXPECT_EQ(total, 7u);
    ASSERT_FALSE(delta.empty());
    // Nothing changed: an empty page. Then one change and one removal.
    auto same = g.handle("GET", delta, std::nullopt);
    ASSERT_TRUE(same);
    EXPECT_TRUE((*same)["value"].empty());
    f.messages_["M1"].modified += 1000;
    f.messages_.erase("M2");
    auto changed = g.handle("GET", jstr(*same, "@odata.deltaLink"), std::nullopt);
    ASSERT_TRUE(changed);
    std::set<std::string> ids, removed;
    for (const auto& x : (*changed)["value"]) (x.contains("@removed") ? removed : ids).insert(jstr(x, "id"));
    EXPECT_EQ(ids, std::set<std::string>{"M1"});
    EXPECT_EQ(removed, std::set<std::string>{"M2"});
}

TEST(OutlookGraph, AnUnreadableItemIsTriedAgainOnlyOnceItChanges) {
    FakeOutlook f;
    const long long now = unix_millis_now();
    f.add("M1", now - kHour, "Readable");
    f.add("M2", now - 2 * kHour, "Encrypted");
    f.unreadable_.insert("M2");
    OutlookGraph g(f, system_clock());
    auto first = g.handle("GET", "/me/mailFolders/F-INBOX/messages/delta", std::nullopt);
    ASSERT_TRUE(first);
    EXPECT_EQ((*first)["value"].size(), 1u);
    const int opened = f.opened;
    auto again = g.handle("GET", jstr(*first, "@odata.deltaLink"), std::nullopt);
    ASSERT_TRUE(again);
    EXPECT_TRUE((*again)["value"].empty());
    EXPECT_EQ(f.opened, opened);  // not opened again: Outlook is not asked to decrypt it every cycle
    f.messages_["M2"].modified = now + 1000;  // Outlook stamps any change with the current time
    f.unreadable_.clear();
    auto changed = g.handle("GET", jstr(*again, "@odata.deltaLink"), std::nullopt);
    ASSERT_TRUE(changed);
    ASSERT_EQ((*changed)["value"].size(), 1u);
    EXPECT_EQ(jstr((*changed)["value"][0], "id"), "M2");
    f.messages_.erase("M2");
    auto gone = g.handle("GET", jstr(*changed, "@odata.deltaLink"), std::nullopt);
    ASSERT_TRUE(gone);
    ASSERT_EQ((*gone)["value"].size(), 1u);
    EXPECT_TRUE((*gone)["value"][0].contains("@removed"));
}

TEST(OutlookGraph, AnUnchangedFolderIsNotListedAgain) {
    FakeOutlook f;
    const long long now = unix_millis_now();
    f.add("M1", now - kHour, "One");
    f.add("M2", now - 2 * kHour, "Two");
    OutlookGraph g(f, system_clock());
    auto first = g.handle("GET", "/me/mailFolders/F-INBOX/messages/delta", std::nullopt);
    ASSERT_TRUE(first);
    EXPECT_EQ((*first)["value"].size(), 2u);
    const int reads = f.row_reads;
    auto same = g.handle("GET", jstr(*first, "@odata.deltaLink"), std::nullopt);
    ASSERT_TRUE(same);
    EXPECT_TRUE((*same)["value"].empty());
    EXPECT_EQ(f.row_reads, reads);  // only the fingerprint was read
    f.add("M3", now, "Three");
    f.messages_.erase("M2");
    auto changed = g.handle("GET", jstr(*same, "@odata.deltaLink"), std::nullopt);
    ASSERT_TRUE(changed);
    EXPECT_GT(f.row_reads, reads);
    std::set<std::string> ids, removed;
    for (const auto& x : (*changed)["value"]) (x.contains("@removed") ? removed : ids).insert(jstr(x, "id"));
    EXPECT_EQ(ids, std::set<std::string>{"M3"});
    EXPECT_EQ(removed, std::set<std::string>{"M2"});
}

TEST(OutlookGraph, AnUnchangedCalendarIsNotReadAgain) {
    FakeOutlook f;
    const long long now = unix_millis_now();
    f.add_event("E1", now + kDay, now + kDay + kHour, "Steering committee");
    f.add_event("E2", now + 2 * kDay, now + 2 * kDay + kHour, "Offsite");
    OutlookGraph g(f, system_clock());
    const auto iso = [](long long ms) { return iso8601_utc(sys_clock::time_point(std::chrono::milliseconds(ms))); };
    const std::string view = "/me/calendarView/delta?startDateTime=" + url_encode(iso(now - 30 * kDay)) +
                             "&endDateTime=" + url_encode(iso(now + 60 * kDay));
    auto first = g.handle("GET", view, std::nullopt);
    ASSERT_TRUE(first) << first.error().message;
    EXPECT_EQ((*first)["value"].size(), 2u);
    const int reads = f.event_reads;
    auto same = g.handle("GET", jstr(*first, "@odata.deltaLink"), std::nullopt);
    ASSERT_TRUE(same);
    EXPECT_TRUE((*same)["value"].empty());
    EXPECT_EQ(f.event_reads, reads);  // only the fingerprint was read
    f.events_["E1"].modified = now + 5000;
    f.events_["E1"].subject = "Steering committee (moved)";
    f.events_.erase("E2");
    auto changed = g.handle("GET", jstr(*same, "@odata.deltaLink"), std::nullopt);
    ASSERT_TRUE(changed);
    EXPECT_EQ(f.event_reads, reads);  // only the changed item was read
    EXPECT_EQ(f.items_read, std::vector<std::string>{"E1"});
    std::set<std::string> ids, removed;
    for (const auto& x : (*changed)["value"]) (x.contains("@removed") ? removed : ids).insert(jstr(x, "id"));
    EXPECT_EQ(ids, std::set<std::string>{"E1"});
    EXPECT_EQ(removed, std::set<std::string>{"E2"});
    // A session without a fingerprint reads the calendar every time, as before.
    f.stamps = false;
    const int before = f.event_reads;
    auto plain = g.handle("GET", jstr(*changed, "@odata.deltaLink"), std::nullopt);
    ASSERT_TRUE(plain);
    EXPECT_TRUE((*plain)["value"].empty());
    EXPECT_GT(f.event_reads, before);
}

TEST(OutlookGraph, AChangedCalendarReadsOnlyTheChangedItems) {
    FakeOutlook f;
    const long long now = unix_millis_now();
    const auto occurrence = [&](const std::string& stamp, long long start) -> OlEvent& {
        OlEvent& e = f.add_event(occurrence_id("S", stamp), start, start + kHour, "Weekly status");
        e.series_id = "S";
        return e;
    };
    occurrence("20261201T1000", now + kDay);
    occurrence("20261208T1000", now + 8 * kDay);
    occurrence("20261215T1000", now + 15 * kDay);
    f.add_event("E1", now + 2 * kDay, now + 2 * kDay + kHour, "Steering committee");
    f.add_event("E2", now + 3 * kDay, now + 3 * kDay + kHour, "Offsite");
    OutlookGraph g(f, system_clock());
    const auto iso = [](long long ms) { return iso8601_utc(sys_clock::time_point(std::chrono::milliseconds(ms))); };
    const std::string view = "/me/calendarView/delta?startDateTime=" + url_encode(iso(now - 30 * kDay)) +
                             "&endDateTime=" + url_encode(iso(now + 60 * kDay));
    auto first = g.handle("GET", view, std::nullopt);
    ASSERT_TRUE(first) << first.error().message;
    EXPECT_EQ((*first)["value"].size(), 5u);
    const int reads = f.event_reads;

    // The series changes: its second occurrence moves and its third is deleted, so every occurrence carries the
    // series' new modification. A single event is deleted; the other single event is unchanged.
    for (auto& [id, e] : f.events_)
        if (e.series_id == "S") e.modified = now + 5000;
    f.events_[occurrence_id("S", "20261208T1000")].start += kHour;
    f.events_[occurrence_id("S", "20261208T1000")].end += kHour;
    f.events_.erase(occurrence_id("S", "20261215T1000"));
    f.events_.erase("E2");
    auto changed = g.handle("GET", jstr(*first, "@odata.deltaLink"), std::nullopt);
    ASSERT_TRUE(changed) << changed.error().message;
    EXPECT_EQ(f.event_reads, reads);  // the window was not read again
    EXPECT_EQ(f.items_read, std::vector<std::string>{"S"});
    std::set<std::string> ids, removed;
    for (const auto& x : (*changed)["value"]) (x.contains("@removed") ? removed : ids).insert(jstr(x, "id"));
    // The first occurrence's change key carries the series' new modification, so it is listed again too.
    EXPECT_EQ(ids, (std::set<std::string>{occurrence_id("S", "20261201T1000"), occurrence_id("S", "20261208T1000")}));
    EXPECT_EQ(removed, (std::set<std::string>{occurrence_id("S", "20261215T1000"), "E2"}));

    // Next cycle, nothing changed: nothing is read or listed, and nothing removed comes back.
    f.items_read.clear();
    auto quiet = g.handle("GET", jstr(*changed, "@odata.deltaLink"), std::nullopt);
    ASSERT_TRUE(quiet);
    EXPECT_TRUE((*quiet)["value"].empty());
    EXPECT_TRUE(f.items_read.empty());

    // A session without the item table reads the whole window, as before.
    f.item_table = false;
    f.events_["E1"].modified = now + 9000;
    auto full = g.handle("GET", jstr(*quiet, "@odata.deltaLink"), std::nullopt);
    ASSERT_TRUE(full);
    EXPECT_GT(f.event_reads, reads);
    std::set<std::string> full_ids;
    for (const auto& x : (*full)["value"]) full_ids.insert(jstr(x, "id"));
    EXPECT_EQ(full_ids, std::set<std::string>{"E1"});
}

// ---- the whole mail stack on the Local Outlook backend ----

TEST(OutlookBackend, SyncBackfillsThenAnnouncesNewChangedAndDeletedMail) {
    OutlookFixture fx;
    fx.fake->add("M1", fx.now - 2 * kHour, "Budget 2027");
    fx.fake->add("M2", fx.now - kHour, "Offsite agenda");
    fx.fake->add_event("E1", fx.now + kDay, fx.now + kDay + kHour, "Steering committee");
    ASSERT_TRUE(fx.svc->connected());
    auto r = fx.svc->sync_now();
    ASSERT_TRUE(r) << r.error().message;
    EXPECT_EQ(fx.svc->store().counts().messages, 2);
    EXPECT_EQ(fx.svc->store().counts().events, 1);
    EXPECT_EQ(fx.svc->account(), "jan@example.com");
    auto ev = fx.take();
    EXPECT_EQ(fx.count(ev, "mail.received"), 0);  // the first read is a backfill
    EXPECT_EQ(fx.count(ev, "mail.received", true), 2);

    fx.fake->add("M3", fx.now, "Please review the contract");
    fx.fake->messages_["M1"].modified += 5000;
    fx.fake->messages_["M1"].is_read = true;
    fx.fake->messages_.erase("M2");
    const int opened_before = fx.fake->opened;
    ASSERT_TRUE(fx.svc->sync_now());
    EXPECT_EQ(fx.fake->opened - opened_before, 2);  // only the new and the changed message are opened
    ev = fx.take();
    EXPECT_EQ(fx.count(ev, "mail.received"), 1);
    EXPECT_EQ(fx.count(ev, "mail.updated"), 1);
    EXPECT_EQ(fx.count(ev, "mail.deleted"), 1);
    EXPECT_EQ(fx.svc->store().counts().messages, 2);
}

// close-replied-work: a new message in Sent Items is "mail sent" (not received); backfill and drafts are not.
TEST(OutlookBackend, MailTheUserSentIsAnnouncedAsSentOutsideBackfill) {
    OutlookFixture fx;
    fx.fake->add("M1", fx.now - 2 * kHour, "Order 4711").conversation_id = "C-ORDER";
    fx.fake->add("S0", fx.now - 3 * kHour, "Earlier reply", "F-SENT").conversation_id = "C-OLD";
    ASSERT_TRUE(fx.svc->sync_now());
    auto ev = fx.take();
    EXPECT_EQ(fx.count(ev, "mail.sent"), 0);  // the first read of Sent Items is a backfill
    EXPECT_EQ(fx.count(ev, "mail.sent", true), 0);
    EXPECT_FALSE(fx.svc->store().user_replied("C-ORDER", fx.now - 2 * kHour));

    auto& draft = fx.fake->add("D9", fx.now - kHour, "RE: Order 4711 (draft)", "F-DRAFTS");
    draft.conversation_id = "C-ORDER";
    draft.is_draft = true;
    fx.fake->add("S1", fx.now - kHour, "RE: Order 4711", "F-SENT").conversation_id = "C-ORDER";
    ASSERT_TRUE(fx.svc->sync_now());
    ev = fx.take();
    EXPECT_EQ(fx.count(ev, "mail.sent"), 1);
    EXPECT_EQ(fx.count(ev, "mail.received"), 0);
    const auto sent = std::find_if(ev.begin(), ev.end(), [](const MailEvent& e) { return e.kind == "mail.sent"; });
    ASSERT_NE(sent, ev.end());
    EXPECT_EQ(sent->id, "S1");
    EXPECT_EQ(jstr(sent->detail, "conversation_id"), "C-ORDER");
    const auto stored = fx.svc->store().message("S1");
    ASSERT_TRUE(stored);
    EXPECT_EQ(jint(sent->detail, "sent_at"), stored->received_at);
    // The reply lookup over the local copy: after the request, not after the reply itself, never another conversation.
    EXPECT_TRUE(fx.svc->store().user_replied("C-ORDER", fx.now - 2 * kHour));
    EXPECT_FALSE(fx.svc->store().user_replied("C-ORDER", stored->received_at));
    EXPECT_FALSE(fx.svc->store().user_replied("C-OTHER", 0));
    EXPECT_FALSE(fx.svc->store().user_replied("", 0));
    // Nothing is announced twice.
    ASSERT_TRUE(fx.svc->sync_now());
    EXPECT_EQ(fx.count(fx.take(), "mail.sent"), 0);
}

TEST(OutlookBackend, RestartKeepsTheDeltaAndAnnouncesNothingTwice) {
    OutlookFixture fx;
    fx.fake->add("M1", fx.now - kHour, "Hello");
    ASSERT_TRUE(fx.svc->sync_now());
    fx.take();
    fx.restart();
    const int opened_before = fx.fake->opened;
    ASSERT_TRUE(fx.svc->sync_now());
    EXPECT_EQ(fx.fake->opened, opened_before);  // the persisted snapshot: nothing re-read
    EXPECT_TRUE(fx.take().empty());
    fx.fake->add("M2", fx.now, "After restart");
    ASSERT_TRUE(fx.svc->sync_now());
    EXPECT_EQ(fx.count(fx.take(), "mail.received"), 1);
}

TEST(OutlookBackend, ExpiredGenerationFallsBackToAFullReadWithoutDuplicates) {
    OutlookFixture fx;
    fx.fake->add("M1", fx.now - kHour, "Hello");
    ASSERT_TRUE(fx.svc->sync_now());
    fx.take();
    fx.svc->connection()->outlook->reset();
    auto r = fx.svc->sync_now();
    ASSERT_TRUE(r) << r.error().message;
    EXPECT_GE(r->resyncs, 1);
    EXPECT_TRUE(fx.take().empty());
}

TEST(OutlookBackend, OutlookClosedServesTheLocalCopy) {
    OutlookFixture fx;
    fx.fake->add("M1", fx.now - kHour, "Quarterly numbers");
    ASSERT_TRUE(fx.svc->sync_now());
    fx.fake->down = true;
    EXPECT_FALSE(fx.svc->sync_now());
    const json out = fx.run("mail.search", {{"query", "Quarterly"}});
    EXPECT_TRUE(jbool(out, "offline", false)) << out.dump();
    ASSERT_EQ(out["results"].size(), 1u);
}

TEST(OutlookBackend, LabelledMessageIsWithheld) {
    OutlookFixture fx(json{{"mail.excluded_labels", json::array({kSecretLabel})}});
    auto& m = fx.fake->add("M1", fx.now - kHour, "Strictly confidential plan");
    m.msip_labels = std::string("MSIP_Label_") + kSecretLabel + "_Enabled=true; MSIP_Label_" + kSecretLabel + "_Name=Secret";
    ASSERT_TRUE(fx.svc->sync_now());
    const auto stored = fx.svc->store().message("M1");
    ASSERT_TRUE(stored);
    EXPECT_TRUE(stored->withheld);
    const json read = fx.run("mail.read", {{"id", "M1"}});
    EXPECT_EQ(jstr(read, "withheld"), "withheld (sensitivity label)");
    EXPECT_FALSE(read.contains("subject"));
}

TEST(OutlookBackend, ExcludedFolderIsNotSynced) {
    OutlookFixture fx(json{{"mail.exclusions.folders", json::array({"Private"})}});
    fx.fake->add("M1", fx.now - kHour, "Personal", "F-PRIV");
    fx.fake->add("M2", fx.now - kHour, "Work");
    ASSERT_TRUE(fx.svc->sync_now());
    EXPECT_FALSE(fx.svc->store().message("M1"));
    EXPECT_TRUE(fx.svc->store().message("M2"));
}

TEST(OutlookBackend, ToolsSearchReadAndReplyThroughOutlook) {
    OutlookFixture fx;
    fx.fake->add("M1", fx.now - kHour, "Cost estimate request");
    json out = fx.run("mail.search", {{"query", "Cost estimate"}});
    ASSERT_EQ(out["results"].size(), 1u) << out.dump();
    ASSERT_TRUE(fx.fake->last_query);
    EXPECT_EQ(fx.fake->last_query->text, "Cost estimate");

    out = fx.run("mail.read", {{"id", "M1"}, {"attachment_id", "1"}});
    EXPECT_EQ(jstr(out, "text"), "hello") << out.dump();

    bool ok = false;
    out = fx.run("mail.send", {{"reply_to", "M1"}, {"to", json::array({"petra@example.com"})}, {"subject", "RE: Cost estimate"}, {"body", "Attached by Friday."}}, &ok);
    ASSERT_TRUE(ok) << out.dump();
    ASSERT_EQ(fx.fake->sent.size(), 1u);
    EXPECT_EQ(fx.fake->sent[0].body, "Attached by Friday.");
    EXPECT_EQ(fx.fake->sent[0].subject, "RE: Cost estimate");
    EXPECT_EQ(fx.fake->writes.front(), "create_reply:M1:reply");
}

TEST(OutlookBackend, CalendarToolsAndIdempotentCreate) {
    OutlookFixture fx;
    auto& e = fx.fake->add_event("SER.20261124T0900", fx.now + kDay, fx.now + kDay + kHour, "Weekly sync");
    e.series_id = "SER";
    json out = fx.run("calendar.list", {});
    ASSERT_EQ(out["events"].size(), 1u) << out.dump();
    EXPECT_EQ(jstr(out["events"][0], "series_master_id"), "SER");

    const json args = {{"subject", "Review"}, {"start", "2027-01-05T09:00:00Z"}, {"end", "2027-01-05T10:00:00Z"},
                       {"attendees", json::array({"petra@example.com"})}};
    bool ok = false;
    out = fx.run("calendar.create", args, &ok, "call-create-1");
    ASSERT_TRUE(ok) << out.dump();
    out = fx.run("calendar.create", args, &ok, "call-create-1");  // a retried tool call
    ASSERT_TRUE(ok) << out.dump();
    EXPECT_EQ(fx.fake->saved_events, 1);

    out = fx.run("calendar.respond", {{"id", "SER.20261124T0900"}, {"response", "tentative"}}, &ok);
    ASSERT_TRUE(ok) << out.dump();
    EXPECT_EQ(fx.fake->writes.back(), "respond:SER.20261124T0900:tentative");

    out = fx.run("calendar.find_free_time", {{"attendees", json::array({"petra@example.com", "nobody@example.com"})},
                                             {"duration_minutes", 30}, {"working_hours_only", false}}, &ok);
    ASSERT_TRUE(ok) << out.dump();
    EXPECT_FALSE(out["slots"].empty());
    EXPECT_EQ(out["free_busy_unknown"], json::array({"nobody@example.com"}));
}

TEST(OutlookBackend, SwitchingBackendRefillsTheCopy) {
    OutlookFixture fx;
    fx.fake->add("M1", fx.now - kHour, "Hello");
    ASSERT_TRUE(fx.svc->sync_now());
    EXPECT_EQ(fx.svc->store().counts().messages, 1);
    fx.policy["mail.backend"] = "graph";
    fx.policy["mail.oauth.authority"] = "http://127.0.0.1:9";
    fx.policy["mail.oauth.tenant_id"] = "t";
    fx.policy["mail.oauth.client_id"] = "c";
    fx.policy["mail.graph_base_url"] = "http://127.0.0.1:9/v1.0";
    fx.svc->policy_changed();
    EXPECT_EQ(fx.svc->config().backend, "graph");
    EXPECT_EQ(fx.svc->store().counts().messages, 0);
}

TEST(OutlookBackend, StatusAndOpenInOutlook) {
    OutlookFixture fx;
    const json s = fx.svc->status();
    EXPECT_EQ(jstr(s, "backend"), "outlook");
    EXPECT_EQ(jstr(s, "outlook_state"), "running");
    EXPECT_FALSE(jbool(s, "outlook_may_start", true));
    ASSERT_TRUE(fx.svc->set_outlook_may_start(true));
    EXPECT_TRUE(fx.svc->outlook_may_start());
    ASSERT_TRUE(fx.svc->open_in_outlook("M9"));
    EXPECT_EQ(fx.fake->writes.back(), "display:M9");
}
