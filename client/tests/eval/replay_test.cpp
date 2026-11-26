// Replay of an evaluation export (client/evaluation Replay Without Side Effects, Replay Variants, Replay Limits And
// Progress), the export job on the pipe, and the speech worker's replay inputs.
#include <gtest/gtest.h>

#include "app/eval_rpc.h"
#include "eval/eval_jobs.h"
#include "eval/jsonl.h"
#include "eval/replay.h"
#include "eval/wav.h"
#include "host/rpc.h"
#include "mail/mail_store.h"
#include "meeting/audio_file.h"
#include "meeting/core.h"
#include "support/dev_endpoints.h"
#include "support/graph_mock.h"

#include <atomic>
#include <fstream>

using namespace wb;
using namespace std::chrono_literals;

namespace {

// Answers triage batches: every event is relevant and needs action; counts requests; can block or fail.
class ReplayModel : public model::IChatModel {
public:
    std::optional<model::RoleInfo> role_info(model::Role role) override {
        if (role != model::Role::small) return std::nullopt;
        model::RoleInfo info;
        info.role = role;
        info.kind = "chat";
        return info;
    }
    model::ModelResult<model::ChatResponse> chat(const model::ChatRequest& r, const model::ChatSink&) override {
        ++requests;
        last_system = r.messages.at(0).content;
        last_feature = r.feature;
        while (hold) std::this_thread::sleep_for(5ms);
        if (fail) return model::ModelFailure{"TIMEOUT", "timed out", "", 0, "", 0, true, false};
        const auto data = parse_json(r.messages.at(1).content);
        json results = json::array();
        if (data && data->contains("events"))
            for (const auto& e : (*data)["events"])
                results.push_back({{"id", jstr(e, "id")}, {"revision", jstr(e, "revision")}, {"outcome", "relevant"},
                                   {"reason", "Asks you"}, {"action", "required"}, {"category", "request"}});
        model::ChatResponse out;
        out.finish_reason = "stop";
        if (drop_one && !results.empty()) results.erase(results.end() - 1);
        out.text = json{{"results", results}}.dump();
        out.usage.prompt = 100;
        out.usage.completion = 20;
        return out;
    }
    std::atomic<int> requests{0};
    std::atomic<bool> hold{false}, fail{false};
    std::string last_system, last_feature;
    std::atomic<bool> drop_one{false};
};

struct Folder {
    std::filesystem::path dir = std::filesystem::temp_directory_path() / ("skowork-replay-" + std::to_string(::GetCurrentProcessId()));
    Folder() {
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
        std::ofstream(dir / "manifest.json") << json{{"complete", true}}.dump() << "\n";
    }
    ~Folder() { std::filesystem::remove_all(dir); }
    void mail(int n) {
        std::ofstream out(dir / "mail.jsonl", std::ios::binary);
        for (int i = 0; i < n; ++i)
            out << json{{"id", "mail:m" + std::to_string(i)},
                        {"item", {{"from", {{"name", "Petra"}, {"address", "petra@example.cz"}}}, {"subject", "Budget"},
                                  {"received_at", 1'790'000'000'000LL}, {"body", "Please review the budget"}, {"to", json::array()}}}}
                       .dump()
                << "\n";
    }
    void meetings(const json& audio) {
        std::ofstream out(dir / "meetings.jsonl", std::ios::binary);
        out << json{{"id", "meeting:mtg1"}, {"item", {{"language", "en"}, {"attendees", {"a@example.cz", "b@example.cz"}}, {"audio", audio}}}}.dump()
            << "\n";
    }
    std::vector<json> results(const std::string& run) const {
        std::vector<json> out;
        std::ifstream in(dir / "runs" / run / "results.jsonl", std::ios::binary);
        std::string line;
        while (std::getline(in, line))
            if (auto j = parse_json(line)) out.push_back(*j);
        return out;
    }
    json run_json(const std::string& run) const {
        std::ifstream in(dir / "runs" / run / "run.json", std::ios::binary);
        std::string line;
        std::getline(in, line);
        auto j = parse_json(line);
        return j ? *j : json();
    }
};

eval::ReplayEnv env_with(ReplayModel& model) {
    eval::ReplayEnv e;
    e.model = &model;
    e.responsibilities = [] { return std::string("Budgets"); };
    e.now = [] { return 1'790'000'000'000LL; };
    e.pause_poll = 20ms;
    return e;
}

eval::ReplayRequest triage_request(const Folder& f, json variant = json::object(), int max = 200) {
    eval::ReplayRequest r;
    r.folder = f.dir;
    r.step = eval::Step::triage;
    r.variant = std::move(variant);
    r.max_items = max;
    return r;
}

}  // namespace

TEST(Replay, TriageWritesResultsOnly) {
    Folder f;
    f.mail(30);
    ReplayModel model;
    eval::Replayer rep(env_with(model));
    auto id = rep.start(triage_request(f));
    ASSERT_TRUE(id) << id.error().message;
    rep.wait();
    const auto lines = f.results(*id);
    ASSERT_EQ(lines.size(), 30u);
    EXPECT_EQ(lines[0]["result"]["action"], "required");
    EXPECT_EQ(model.requests.load(), 2);  // batches of 20
    const json run = f.run_json(*id);
    EXPECT_EQ(run["items"], 30);
    EXPECT_EQ(run["state"], "done");
    EXPECT_EQ(run["instruction_id"], eval::Variant::defaults(eval::Step::triage).instruction_id());
    EXPECT_EQ(model.last_system, eval::Variant::defaults(eval::Step::triage).text("triage.instructions"));
    EXPECT_EQ(rep.status()["state"], "done");
}

TEST(Replay, VariantAppliesAndUnknownIsRejectedBeforeFirstItem) {
    Folder f;
    f.mail(5);
    ReplayModel model;
    eval::Replayer rep(env_with(model));
    auto bad = rep.start(triage_request(f, {{"triage.instructons", "x"}}));
    ASSERT_FALSE(bad);
    EXPECT_EQ(model.requests.load(), 0);
    auto good = rep.start(triage_request(f, {{"triage.instructions", "Be brief."}, {"triage.batch_size", 2}}));
    ASSERT_TRUE(good);
    rep.wait();
    EXPECT_EQ(model.last_system, "Be brief.");
    EXPECT_EQ(model.requests.load(), 3);
    EXPECT_NE(f.run_json(*good)["instruction_id"], eval::Variant::defaults(eval::Step::triage).instruction_id());
}

TEST(Replay, MaxItemsBounds) {
    Folder f;
    const std::string dir = meeting::path_to_utf8(f.dir);
    EXPECT_FALSE(eval::replay_request_from_json({{"folder", dir}, {"step", "triage"}, {"max_items", 0}}));
    EXPECT_FALSE(eval::replay_request_from_json({{"folder", dir}, {"step", "triage"}, {"max_items", 2001}}));
    EXPECT_FALSE(eval::replay_request_from_json({{"folder", dir}, {"step", "triagee"}}));
    auto ok = eval::replay_request_from_json({{"folder", dir}, {"step", "triage"}});
    ASSERT_TRUE(ok);
    EXPECT_EQ(ok->max_items, 200);
    f.mail(10);
    ReplayModel model;
    eval::Replayer rep(env_with(model));
    auto id = rep.start(triage_request(f, json::object(), 3));
    ASSERT_TRUE(id);
    rep.wait();
    EXPECT_EQ(f.results(*id).size(), 3u);
}

TEST(Replay, BusyAndCancelKeepsWritten) {
    Folder f;
    f.mail(60);
    ReplayModel model;
    eval::Replayer rep(env_with(model));
    auto id = rep.start(triage_request(f, {{"triage.batch_size", 1}}));
    ASSERT_TRUE(id);
    for (int i = 0; i < 400 && model.requests.load() < 5; ++i) std::this_thread::sleep_for(5ms);
    model.hold = true;
    auto second = rep.start(triage_request(f));
    ASSERT_FALSE(second);
    EXPECT_EQ(second.error().code, "busy");
    rep.cancel();
    model.hold = false;
    rep.wait();
    const auto written = f.results(*id).size();
    EXPECT_GE(written, 4u);
    EXPECT_LT(written, 60u);
    EXPECT_EQ(rep.status()["state"], "cancelled");
}

TEST(Replay, FailuresAreCountedByKind) {
    Folder f;
    f.mail(3);
    ReplayModel model;
    model.fail = true;
    eval::Replayer rep(env_with(model));
    auto id = rep.start(triage_request(f));
    ASSERT_TRUE(id);
    rep.wait();
    EXPECT_EQ(f.run_json(*id)["failures_by_kind"]["unavailable"], 3);
    EXPECT_EQ(f.results(*id)[0]["error"]["kind"], "unavailable");
}

TEST(Replay, AttendeesClusterCount) {
    EXPECT_EQ(eval::cluster_count("attendees", {"Jan Novák", "a", "b", "c"}, "Jan Novák"), 3);
    EXPECT_EQ(eval::cluster_count("attendees", {"Jan Novák", "a"}, "Jan Novák"), -1);
    EXPECT_EQ(eval::cluster_count("auto", {"a", "b", "c"}, "x"), -1);
}

TEST(Replay, SkipsMeetingWithoutAudioAndPausesDuringCapture) {
    Folder f;
    f.meetings("unavailable");
    ReplayModel model;
    auto e = env_with(model);
    std::atomic<int> speech_calls{0};
    e.speech = [&](const json&) -> Result<json> {
        ++speech_calls;
        return json{{"language", "en"}, {"segments", json::array()}};
    };
    std::atomic<bool> busy{false};
    e.speech_busy = [&] { return busy.load(); };
    {
        eval::Replayer rep(e);
        eval::ReplayRequest r;
        r.folder = f.dir;
        r.step = eval::Step::diarize;
        auto id = rep.start(r);
        ASSERT_TRUE(id);
        rep.wait();
        EXPECT_EQ(f.run_json(*id)["failures_by_kind"]["no_audio"], 1);
        EXPECT_EQ(speech_calls.load(), 0);
    }
    f.meetings({{"remote", "audio/mtg1/remote.wav"}});
    busy = true;
    eval::Replayer rep(e);
    eval::ReplayRequest r;
    r.folder = f.dir;
    r.step = eval::Step::asr;
    r.variant = {{"asr.beam_paths", 2}};
    auto id = rep.start(r);
    ASSERT_TRUE(id);
    for (int i = 0; i < 400 && rep.status()["state"] != "paused"; ++i) std::this_thread::sleep_for(5ms);
    EXPECT_EQ(rep.status()["state"], "paused");
    EXPECT_EQ(speech_calls.load(), 0);
    busy = false;
    rep.wait();
    EXPECT_EQ(speech_calls.load(), 1);
    EXPECT_EQ(f.results(*id).size(), 1u);
}

TEST(SttRequest, SettingsDefaultsAndOverrides) {
    const auto d = meeting::engine_settings_from_json(json::object());
    EXPECT_EQ(d.vad_threshold, 0.5f);
    EXPECT_EQ(d.diarize_threshold, 0.5f);
    EXPECT_EQ(d.num_clusters, -1);
    EXPECT_EQ(d.max_active_paths, 4);
    EXPECT_EQ(d.hotwords_score, 2.0f);
    const auto o = meeting::engine_settings_from_json({{"threshold", 0.7}, {"num_clusters", 3}, {"max_active_paths", 2}, {"hotwords_score", 9.0}});
    EXPECT_FLOAT_EQ(o.diarize_threshold, 0.7f);
    EXPECT_EQ(o.num_clusters, 3);
    EXPECT_EQ(o.max_active_paths, 2);
    EXPECT_EQ(o.hotwords_score, 2.0f);  // out of range: the default stays
}

TEST(SttRequest, AcceptsExportedWavTracks) {
    WB_REQUIRES_DEV_ENDPOINTS();
    Folder f;
    std::vector<std::int16_t> pcm(16000 * 2);
    for (size_t i = 0; i < pcm.size(); ++i) pcm[i] = static_cast<std::int16_t>((i % 80) * 300);
    ASSERT_TRUE(eval::write_wav(f.dir / "remote.wav", pcm, 16000));
    auto r = meeting::stt_request("stt.transcribe", {{"tracks", {meeting::path_to_utf8(f.dir / "remote.wav")}}, {"engine", "test"},
                                                     {"settings", {{"num_clusters", 2}}}});
    ASSERT_TRUE(r) << r.error().message;
    EXPECT_TRUE(r->contains("segments"));
    ASSERT_TRUE(eval::write_wav(f.dir / "other.wav", pcm, 16000));
    EXPECT_FALSE(meeting::stt_request("stt.transcribe", {{"tracks", {meeting::path_to_utf8(f.dir / "other.wav")}}, {"engine", "test"}}));
}

TEST(EvalRpc, ExportJobLifecycle) {
    MailDb db;
    mail::MailStore store(db.store->db());
    ASSERT_TRUE(store.set_folders({{"f-inbox", "Inbox", "inbox"}}));
    for (int i = 0; i < 3; ++i) {
        mail::StoredMessage m;
        m.id = "m" + std::to_string(i);
        m.folder_id = "f-inbox";
        m.change_key = "k";
        m.subject = "S";
        m.received_at = 1'790'000'000'000LL + i;
        store.upsert_message(m);
    }
    eval::ExportEnv xe;
    xe.mail = [&] { return &store; };
    xe.app_db = &db.store->db();
    xe.now = [] { return 1'790'000'000'000LL; };
    ReplayModel model;
    eval::EvalJobs jobs(xe, env_with(model));
    RpcRegistry rpc;
    register_eval_rpc(rpc, [&] { return static_cast<eval::EvalService*>(&jobs); }, [] { return true; });
    Folder f;
    const json params = {{"out_dir", meeting::path_to_utf8(f.dir / "exports")}, {"from", "2026-09-01"}, {"to", "2026-10-01"}, {"sources", {"mail"}}};
    json dry_params = params;
    dry_params["dry_run"] = true;
    auto dry = (*rpc.find("eval.export", true))(dry_params);
    ASSERT_TRUE(dry);
    EXPECT_EQ((*dry)["audio_bytes"], 0);
    auto started = (*rpc.find("eval.export", true))(params);
    ASSERT_TRUE(started) << started.error().message;
    jobs.wait();
    auto st = (*rpc.find("eval.status", true))({{"job", (*started)["job"]}});
    ASSERT_TRUE(st);
    EXPECT_EQ((*st)["state"], "done");
    EXPECT_EQ((*st)["counts"]["mail"]["written"], 3);
    const std::filesystem::path folder = meeting::path_from_utf8(jstr(*started, "folder"));
    std::ifstream in(folder / "manifest.json");
    std::string line;
    std::getline(in, line);
    EXPECT_TRUE((*parse_json(line))["complete"].get<bool>());
    EXPECT_FALSE((*rpc.find("eval.status", true))({{"job", "nope"}}));
}

// Review fix: a continued capture's later parts (remote.2.wav) are named by their kind, and a replay sends each
// kind's first part only, so such a meeting replays instead of failing.
TEST(Replay, ContinuedPartsDoNotFailTheMeeting) {
    WB_REQUIRES_DEV_ENDPOINTS();
    Folder f;
    std::vector<std::int16_t> pcm(16000);
    ASSERT_TRUE(eval::write_wav(f.dir / "remote.2.wav", pcm, 16000));
    EXPECT_TRUE(meeting::stt_request("stt.transcribe", {{"tracks", {meeting::path_to_utf8(f.dir / "remote.2.wav")}}, {"engine", "test"}}));
    f.meetings({{"remote", "audio/mtg1/remote.wav"}, {"remote.2", "audio/mtg1/remote.2.wav"}});
    ReplayModel model;
    auto e = env_with(model);
    size_t tracks = 0;
    e.speech = [&](const json& req) -> Result<json> {
        tracks = req["tracks"].size();
        return json{{"language", "en"}, {"segments", json::array()}};
    };
    eval::Replayer rep(e);
    eval::ReplayRequest r;
    r.folder = f.dir;
    r.step = eval::Step::asr;
    ASSERT_TRUE(rep.start(r));
    rep.wait();
    EXPECT_EQ(tracks, 1u);
}

// Review minor 14: replay usage is measured and attributed to evaluation, a result the model leaves out is a failure,
// and a transcription replay does not ask for diarization.
TEST(Replay, RecordsTokensAndUsesTheEvaluationFeature) {
    Folder f;
    f.mail(3);
    ReplayModel model;
    eval::Replayer rep(env_with(model));
    auto id = rep.start(triage_request(f));
    ASSERT_TRUE(id);
    rep.wait();
    const json run = f.run_json(*id);
    EXPECT_EQ(run["tokens_in"], 100);
    EXPECT_EQ(run["tokens_out"], 20);
    EXPECT_EQ(model.last_feature, "eval.triage");
}

TEST(Replay, ItemsTheModelLeavesOutAreFailures) {
    Folder f;
    f.mail(3);
    ReplayModel model;
    model.drop_one = true;
    eval::Replayer rep(env_with(model));
    auto id = rep.start(triage_request(f));
    ASSERT_TRUE(id);
    rep.wait();
    EXPECT_EQ(f.run_json(*id)["failures_by_kind"]["mismatch"], 3);
    EXPECT_EQ(f.results(*id).size(), 3u);
}

TEST(Replay, TranscriptionDoesNotDiarize) {
    Folder f;
    f.meetings({{"remote", "audio/mtg1/remote.wav"}});
    ReplayModel model;
    auto e = env_with(model);
    std::vector<bool> diarize;
    e.speech = [&](const json& req) -> Result<json> {
        diarize.push_back(jbool(req, "diarize"));
        return json{{"language", "en"}, {"segments", json::array()}};
    };
    for (const auto step : {eval::Step::asr, eval::Step::diarize}) {
        eval::Replayer rep(e);
        eval::ReplayRequest r;
        r.folder = f.dir;
        r.step = step;
        ASSERT_TRUE(rep.start(r));
        rep.wait();
    }
    ASSERT_EQ(diarize.size(), 2u);
    EXPECT_FALSE(diarize[0]);
    EXPECT_TRUE(diarize[1]);
}

namespace {

struct Jobs {
    MailDb db;
    mail::MailStore store{db.store->db()};
    ReplayModel model;
    std::atomic<bool> hold{false};
    std::function<void()> on_mail;
    std::unique_ptr<eval::EvalJobs> jobs;
    Folder f;
    Jobs() {
        EXPECT_TRUE(store.set_folders({{"f-inbox", "Inbox", "inbox"}}));
        mail::StoredMessage m;
        m.id = "m1";
        m.folder_id = "f-inbox";
        m.change_key = "k";
        m.received_at = 1'790'000'000'000LL;
        store.upsert_message(m);
        eval::ExportEnv xe;
        xe.mail = [this] {
            while (hold) std::this_thread::sleep_for(5ms);
            if (on_mail) on_mail();
            return &store;
        };
        xe.app_db = &db.store->db();
        xe.now = [] { return 1'790'000'000'000LL; };
        jobs = std::make_unique<eval::EvalJobs>(xe, env_with(model));
    }
    json export_params() const {
        return {{"out_dir", meeting::path_to_utf8(f.dir / "exports")}, {"from", "2026-09-01"}, {"to", "2026-10-01"}, {"sources", {"mail"}}};
    }
};

}  // namespace

// Review minor 7: an export and a replay never run at once, whichever starts first.
TEST(EvalJobs, OneJobAtATimeBothWays) {
    Jobs j;
    j.f.mail(5);
    j.hold = true;
    auto exp = j.jobs->start_export(j.export_params());
    ASSERT_TRUE(exp) << exp.error().message;
    auto run = j.jobs->start_run({{"folder", meeting::path_to_utf8(j.f.dir)}, {"step", "triage"}});
    ASSERT_FALSE(run);
    EXPECT_EQ(run.error().code, "busy");
    j.hold = false;
    j.jobs->wait();
    j.model.hold = true;
    auto run2 = j.jobs->start_run({{"folder", meeting::path_to_utf8(j.f.dir)}, {"step", "triage"}});
    ASSERT_TRUE(run2) << run2.error().message;
    for (int i = 0; i < 400 && j.model.requests.load() == 0; ++i) std::this_thread::sleep_for(5ms);
    auto exp2 = j.jobs->start_export(j.export_params());
    ASSERT_FALSE(exp2);
    EXPECT_EQ(exp2.error().code, "busy");
    j.model.hold = false;
    j.jobs->wait();
}

// Review minor 8: when the final manifest cannot be written, the job fails instead of reporting done.
TEST(EvalJobs, AManifestThatCannotBeWrittenFailsTheJob) {
    Jobs j;
    std::filesystem::path folder;
    j.on_mail = [&] {
        // The manifest's place is taken by a folder, so it cannot be rewritten at the end.
        std::filesystem::remove(folder / "manifest.json");
        std::filesystem::create_directory(folder / "manifest.json");
    };
    auto exp = j.jobs->start_export(j.export_params());
    ASSERT_TRUE(exp);
    folder = meeting::path_from_utf8(jstr(*exp, "folder"));
    j.jobs->wait();
    auto st = j.jobs->status(jstr(*exp, "job"));
    ASSERT_TRUE(st);
    EXPECT_EQ((*st)["state"], "failed");
}

// Review minor 13: turning the setting off stops a running job.
TEST(EvalJobs, CancelAllStopsARunningJob) {
    Jobs j;
    j.f.mail(40);
    auto run = j.jobs->start_run({{"folder", meeting::path_to_utf8(j.f.dir)}, {"step", "triage"}, {"variant", {{"triage.batch_size", 1}}}});
    ASSERT_TRUE(run);
    for (int i = 0; i < 400 && j.model.requests.load() < 2; ++i) std::this_thread::sleep_for(5ms);
    j.jobs->cancel_all();
    j.jobs->wait();
    auto st = j.jobs->status(jstr(*run, "job"));
    ASSERT_TRUE(st);
    EXPECT_EQ((*st)["state"], "cancelled");
}
