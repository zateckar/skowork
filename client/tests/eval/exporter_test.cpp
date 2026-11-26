// Evaluation export (client/evaluation Export Of Items, Verdicts And Decisions, Export Of Meetings, Export Manifest).
#include <gtest/gtest.h>

#include "eval/decision_log.h"
#include "eval/exporter.h"
#include "eval/jsonl.h"
#include "eval/wav.h"
#include "mail/mail_store.h"
#include "meeting/audio_file.h"
#include "meeting/meeting_store.h"
#include "support/graph_mock.h"

#include <fstream>
#include <sstream>

using namespace wb;
using eval::ExportRequest;

namespace {

constexpr long long kHour = 3'600'000LL;
constexpr long long kT0 = 1'790'000'000'000LL;

std::vector<json> lines(const std::filesystem::path& file) {
    std::vector<json> out;
    std::ifstream in(file, std::ios::binary);
    std::string line;
    while (std::getline(in, line)) {
        auto j = parse_json(line);
        EXPECT_TRUE(j) << line.substr(0, 120);
        if (j) out.push_back(*j);
    }
    return out;
}

std::string dump_table(Database& db, const std::string& table) {
    auto q = db.prepare("SELECT * FROM " + table + " ORDER BY 1");
    std::string out;
    while (q.step())
        for (int i = 0; i < 9; ++i) out += q.col_text(i) + "|";
    return out;
}

struct Rig {
    MailDb db;
    std::unique_ptr<mail::MailStore> mail;
    std::unique_ptr<meeting::MeetingStore> meetings;
    std::unique_ptr<eval::DecisionLog> log;
    std::filesystem::path out = std::filesystem::temp_directory_path() / ("skowork-export-" + std::to_string(::GetCurrentProcessId()));
    long long now = kT0 + 100 * kHour;

    Rig() {
        std::filesystem::remove_all(out);
        mail = std::make_unique<mail::MailStore>(db.store->db());
        EXPECT_TRUE(mail->set_folders({{"f-inbox", "Inbox", "inbox"}, {"f-sent", "Sent Items", "sentitems"}}));
        meetings = std::make_unique<meeting::MeetingStore>(db.store->db());
        log = std::make_unique<eval::DecisionLog>(db.store->db());
    }
    ~Rig() { std::filesystem::remove_all(out); }

    void message(const std::string& id, long long at, const std::string& folder = "f-inbox", const std::string& conversation = "c1",
                 bool withheld = false, const std::string& body = "Please review the budget") {
        mail::StoredMessage m;
        m.id = id;
        m.folder_id = folder;
        m.conversation_id = conversation;
        m.change_key = id + "k";
        m.subject = "Budget " + id;
        m.body = body;
        m.from = {"Petra Malá", "petra.mala@example.cz"};
        m.to = {{"Jan Novák", "jan.novak@example.cz"}};
        m.received_at = at;
        m.withheld = withheld;
        mail->upsert_message(m);
    }
    eval::Exporter exporter() {
        eval::ExportEnv e;
        e.mail = mail.get();
        e.meetings = meetings.get();
        e.app_db = &db.store->db();
        e.decisions = log.get();
        e.facts_of = [](const std::string&, const std::string&) { return json::array({{{"statement", "Petra owns the budget"}}}); };
        e.now = [this] { return now; };
        e.client_version = "0.0.0-test";
        return eval::Exporter(std::move(e));
    }
    ExportRequest request(std::set<std::string> sources, bool audio = false) {
        ExportRequest r;
        r.out_dir = out;
        r.from_ms = kT0;
        r.to_ms = kT0 + 48 * kHour;
        r.sources = std::move(sources);
        r.audio = audio;
        return r;
    }
};

}  // namespace

TEST(Exporter, WindowAndSources) {
    Rig r;
    r.message("m1", kT0 + kHour);
    r.message("m2", kT0 + 2 * kHour);
    r.message("old", kT0 - kHour);
    auto ex = r.exporter();
    const auto req = r.request({"mail"});
    auto folder = ex.begin(req);
    ASSERT_TRUE(folder) << folder.error().message;
    CancelToken cancel;
    auto n = ex.export_mail(*folder, req, cancel);
    ASSERT_TRUE(n);
    EXPECT_EQ(n->written, 2);
    const auto recs = lines(*folder / "mail.jsonl");
    ASSERT_EQ(recs.size(), 2u);
    EXPECT_EQ(recs[0]["id"], "mail:m1");
    EXPECT_EQ(recs[0]["item"]["to"][0]["address"], "jan.novak@example.cz");
    EXPECT_EQ(recs[0]["skowork"]["facts"][0]["statement"], "Petra owns the budget");
    EXPECT_FALSE(std::filesystem::exists(*folder / "calendar.jsonl"));
}

TEST(Exporter, WithheldOnlyCounted) {
    Rig r;
    r.message("m1", kT0 + kHour);
    r.message("secret", kT0 + kHour, "f-inbox", "c2", true);
    auto ex = r.exporter();
    const auto req = r.request({"mail"});
    auto folder = ex.begin(req);
    ASSERT_TRUE(folder);
    CancelToken cancel;
    auto n = ex.export_mail(*folder, req, cancel);
    ASSERT_TRUE(n);
    EXPECT_EQ(n->written, 1);
    EXPECT_EQ(n->withheld, 1);
    for (const auto& rec : lines(*folder / "mail.jsonl")) EXPECT_NE(rec["id"], "mail:secret");
}

TEST(Exporter, UserRepliedDerivedAndSentMailMarked) {
    Rig r;
    r.message("m1", kT0 + kHour, "f-inbox", "c1");
    r.message("reply", kT0 + 2 * kHour, "f-sent", "c1");
    r.message("m2", kT0 + kHour, "f-inbox", "c9");
    auto ex = r.exporter();
    const auto req = r.request({"mail"});
    auto folder = ex.begin(req);
    ASSERT_TRUE(folder);
    CancelToken cancel;
    ASSERT_TRUE(ex.export_mail(*folder, req, cancel));
    std::map<std::string, json> by_id;
    for (const auto& rec : lines(*folder / "mail.jsonl")) by_id[rec["id"].get<std::string>()] = rec;
    EXPECT_TRUE(by_id["mail:m1"]["item"]["user_replied"].get<bool>());
    EXPECT_TRUE(by_id["mail:m1"]["item"]["user_replied_approximate"].get<bool>());
    EXPECT_FALSE(by_id["mail:m2"]["item"]["user_replied"].get<bool>());
    EXPECT_TRUE(by_id["mail:reply"]["item"]["sent_by_user"].get<bool>());
    EXPECT_FALSE(by_id["mail:m1"]["item"]["sent_by_user"].get<bool>());
}

TEST(Exporter, IncludesDecisionsAndLegacyAndChangesNothing) {
    Rig r;
    r.message("m1", kT0 + kHour);
    auto& db = r.db.store->db();
    ASSERT_TRUE(db.exec("INSERT INTO triage_items(scope, item_id, revision, state, outcome, suggestion, queued_at, updated_at, action, "
                        "category) VALUES('mail', 'm1', 'r1', 'evaluated', 'relevant', 'dismissed', 1, 5, 'required', 'request')"));
    eval::Decision d;
    d.at_ms = kT0 + 3 * kHour;
    d.step = "triage";
    d.kind = "dismiss";
    d.item_ref = "mail:m1";
    d.result_ref = "triage:mail/m1";
    ASSERT_TRUE(r.log->record(d));
    const std::string before = dump_table(db, "triage_items") + dump_table(db, "decision_log") + dump_table(db, "mail_messages");
    auto ex = r.exporter();
    const auto req = r.request({"mail", "calendar", "meetings"});
    auto folder = ex.begin(req);
    ASSERT_TRUE(folder);
    CancelToken cancel;
    ASSERT_TRUE(ex.export_mail(*folder, req, cancel));
    ASSERT_TRUE(ex.export_calendar(*folder, req, cancel));
    ASSERT_TRUE(ex.export_meetings(*folder, req, cancel));
    const auto recs = lines(*folder / "mail.jsonl");
    ASSERT_EQ(recs.size(), 1u);
    EXPECT_EQ(recs[0]["skowork"]["triage"]["action"], "required");
    EXPECT_EQ(recs[0]["user"]["decisions"].size(), 1u);
    EXPECT_EQ(recs[0]["user"]["decisions"][0]["kind"], "dismiss");
    EXPECT_EQ(recs[0]["user"]["legacy"]["suggestion"], "dismissed");
    EXPECT_EQ(recs[0]["user"]["legacy"]["at"], 5);
    const std::string after = dump_table(db, "triage_items") + dump_table(db, "decision_log") + dump_table(db, "mail_messages");
    EXPECT_EQ(before, after);
}

TEST(Exporter, FolderExistsFails) {
    Rig r;
    auto ex = r.exporter();
    const auto req = r.request({"mail"});
    ASSERT_TRUE(ex.begin(req));
    auto again = ex.begin(req);
    ASSERT_FALSE(again);
    EXPECT_EQ(again.error().code, "exists");
    ExportRequest bad = req;
    bad.out_dir = "relative/path";
    EXPECT_FALSE(ex.begin(bad));
    bad = req;
    bad.sources = {"mailbox"};
    EXPECT_FALSE(ex.begin(bad));
}

TEST(Exporter, MeetingsWithAndWithoutAudio) {
    Rig r;
    const auto dir = r.out / "capture-src" / "mtg1";
    std::filesystem::create_directories(dir);
    std::vector<std::int16_t> pcm(16000);
    for (size_t i = 0; i < pcm.size(); ++i) pcm[i] = static_cast<std::int16_t>((i % 100) * 50);
    {
        auto w = meeting::TrackWriter::create(dir / "remote.skau", meeting::TrackKind::remote, 16000, 0);
        ASSERT_TRUE(w);
        ASSERT_TRUE((*w)->append(pcm.data(), pcm.size()));
        ASSERT_TRUE((*w)->close());
    }
    meeting::MeetingRecord m;
    m.id = "mtg1";
    m.state = "done";
    m.started_at = kT0 + kHour;
    m.audio_dir = meeting::path_to_utf8(dir);
    m.speakers = {{"me", "Jan Novák"}, {"s1", "Speaker 1"}};
    m.speaker_meta = {{"me", {"auto", 0}}};
    m.attendees = {"petra.mala@example.cz"};
    ASSERT_TRUE(r.meetings->put(m));
    meeting::MeetingRecord gone = m;
    gone.id = "mtg2";
    gone.audio_deleted = true;
    ASSERT_TRUE(r.meetings->put(gone));

    auto ex = r.exporter();
    auto req = r.request({"meetings"}, true);
    EXPECT_GT(ex.audio_size(req), 32000);
    auto folder = ex.begin(req);
    ASSERT_TRUE(folder);
    CancelToken cancel;
    auto n = ex.export_meetings(*folder, req, cancel);
    ASSERT_TRUE(n) << n.error().message;
    EXPECT_EQ(n->written, 2);
    std::map<std::string, json> by_id;
    for (const auto& rec : lines(*folder / "meetings.jsonl")) by_id[rec["id"].get<std::string>()] = rec;
    EXPECT_EQ(by_id["meeting:mtg1"]["item"]["audio"]["remote"], "audio/mtg1/remote.wav");
    EXPECT_EQ(by_id["meeting:mtg1"]["item"]["speaker_meta"]["s1"]["by"], "unknown");
    EXPECT_EQ(by_id["meeting:mtg2"]["item"]["audio"], "unavailable");
    auto back = eval::read_wav(*folder / "audio" / "mtg1" / "remote.wav");
    ASSERT_TRUE(back);
    EXPECT_EQ(*back, pcm);

    r.now += 1000;  // a new folder
    req.audio = false;
    EXPECT_EQ(ex.audio_size(req), 0);
    auto plain = ex.begin(req);
    ASSERT_TRUE(plain);
    ASSERT_TRUE(ex.export_meetings(*plain, req, cancel));
    EXPECT_FALSE(std::filesystem::exists(*plain / "audio"));
    for (const auto& rec : lines(*plain / "meetings.jsonl"))
        if (rec["id"] == "meeting:mtg1") EXPECT_EQ(rec["item"]["audio"], "not_requested");
}

TEST(Exporter, ManifestCountsAndIds) {
    Rig r;
    auto ex = r.exporter();
    const auto req = r.request({"mail"});
    auto folder = ex.begin(req);
    ASSERT_TRUE(folder);
    ASSERT_TRUE(ex.write_manifest(*folder, req, {{"mail", {3, 1}}}, false));
    const auto m = lines(*folder / "manifest.json");
    ASSERT_EQ(m.size(), 1u);
    EXPECT_EQ(m[0]["counts"]["mail"]["written"], 3);
    EXPECT_EQ(m[0]["counts"]["mail"]["withheld"], 1);
    EXPECT_EQ(m[0]["instruction_ids"]["triage"].get<std::string>().size(), 12u);
    EXPECT_FALSE(m[0]["complete"].get<bool>());
    EXPECT_EQ(m[0]["client_version"], "0.0.0-test");
}

TEST(Jsonl, HostileTextIsOneValidLine) {
    const std::string lone_surrogate = "a\xED\xA0\x80z";
    const std::string with_nul = std::string("x\0y", 3);
    const std::string big(5 * 1024 * 1024, 'b');
    const std::string newlines = "line1\nline2\r\nline3";
    for (const auto& text : {lone_surrogate, with_nul, big, newlines, std::string("\xC3")}) {
        const std::string line = eval::jsonl_line(json{{"subject", text}, {"body", text}});
        EXPECT_EQ(line.find('\n'), std::string::npos);
        EXPECT_TRUE(parse_json(line));
    }
}

TEST(Wav, RoundTrip) {
    const auto file = std::filesystem::temp_directory_path() / "skowork-wav-test.wav";
    std::vector<std::int16_t> pcm = {0, 1, -1, 32767, -32768, 1234};
    ASSERT_TRUE(eval::write_wav(file, pcm, 16000));
    int rate = 0;
    auto back = eval::read_wav(file, &rate);
    ASSERT_TRUE(back);
    EXPECT_EQ(*back, pcm);
    EXPECT_EQ(rate, 16000);
    std::filesystem::remove(file);
}
