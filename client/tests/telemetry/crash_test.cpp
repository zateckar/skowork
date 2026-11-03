// Crash capture (add-telemetry-and-admin task 3.5; client/telemetry Crash Reports): an induced worker crash leaves a
// filtered dump in the file the core passed down; it is imported, summarised, held for review, sent after the hour,
// and can be deleted; with crash reports disabled nothing is written.
#include "support/dev_endpoints.h"
#include "support/runtime_harness.h"
#include "host/workers.h"
#include "platform/crash.h"
#include "sandbox/proxy.h"
#include "telemetry/service.h"
#include "tools/extractor.h"

#include <gtest/gtest.h>

#include <fstream>

using namespace wb;
using namespace wb::telemetry;

namespace {

struct CrashRig {
    std::filesystem::path dir = std::filesystem::temp_directory_path() / L"workbench-test" / widen("crash-" + random_urlsafe(6));
    std::filesystem::path dumps = dir / L"crashes";
    FakeClock clock;
    std::shared_ptr<const policy::EffectivePolicy> pol;
    std::vector<std::pair<json, size_t>> uploads;
    std::unique_ptr<TelemetryService> svc;
    std::vector<std::pair<std::filesystem::path, std::string>> crashes;

    CrashRig() {
        std::filesystem::create_directories(dir);
        db_key key{};
        for (size_t i = 0; i < key.size(); ++i) key[i] = static_cast<std::uint8_t>(i + 3);
        TelemetryEnv env;
        env.policy = [this] { return pol; };
        env.send = [](const std::string&) -> Result<HttpResponse> { return HttpResponse{200}; };
        env.send_crash = [this](const json& meta, const std::string& dump) -> Result<HttpResponse> {
            uploads.emplace_back(meta, dump.size());
            return HttpResponse{202};
        };
        env.installation_id = [] { return std::string("inst-1"); };
        env.clock = &clock;
        auto s = TelemetryService::open(dir / L"telemetry-buffer.db", key, std::move(env));
        EXPECT_TRUE(s);
        if (s) svc = std::move(*s);
    }
    ~CrashRig() {
        svc.reset();
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
    void set_crash_reports(bool on) {
        policy::PolicyBundle b;
        b.sub = "user";
        b.ver = 3;
        b.exp = 4102444800;
        b.policy = {{"telemetry", {{"crash_reports", on}}}};
        pol = std::make_shared<const policy::EffectivePolicy>(b);
    }
    WorkerHost::Options worker_options() {
        WorkerHost::Options o;
        o.worker_exe = exe_dir() / L"workbench-worker.exe";
        o.crash_dir = [this]() -> std::optional<std::filesystem::path> {
            if (!svc->crash_reports_enabled()) return std::nullopt;
            return dumps;
        };
        o.on_crash = [this](const std::filesystem::path& d, const std::string& p) { crashes.emplace_back(d, p); };
        return o;
    }
    size_t files() const {
        std::error_code ec;
        size_t n = 0;
        for ([[maybe_unused]] const auto& e : std::filesystem::directory_iterator(dumps, ec)) ++n;
        return n;
    }
};

}  // namespace

TEST(CrashReports, AnInducedWorkerCrashIsCapturedHeldSentAndDeletable) {
    WB_REQUIRES_DEV_ENDPOINTS();
    CrashRig rig;
    {
        WorkerHost host(rig.worker_options());
        ASSERT_TRUE(host.call(WorkerRole::index, "ping", json::object()));
        auto r = host.call(WorkerRole::index, "crash", json::object());
        ASSERT_FALSE(r);
        EXPECT_EQ(r.error().code, "worker_failed");
    }
    ASSERT_EQ(rig.crashes.size(), 1u);
    const auto& [dump, process] = rig.crashes[0];
    EXPECT_EQ(process, "worker-index");
    auto s = crash::summarize(dump);
    ASSERT_TRUE(s) << s.error().message;
    EXPECT_EQ(s->exception_code, "0xC000001D");  // __builtin_trap: illegal instruction
    EXPECT_EQ(s->module, "workbench-worker.exe");
    EXPECT_GT(s->threads, 0);
    ASSERT_FALSE(s->modules.empty());
    for (const auto& m : s->modules) EXPECT_EQ(m.find_first_of("\\/"), std::string::npos) << m;  // no paths

    rig.svc->capture_crash(dump, process);
    EXPECT_FALSE(std::filesystem::exists(dump));  // the raw file is gone once imported
    json v = rig.svc->view();
    ASSERT_EQ(v["crashes"].size(), 1u);
    const std::string id = jstr(v["crashes"][0], "id");
    EXPECT_EQ(jstr(v["crashes"][0], "state"), "held");
    EXPECT_EQ(jstr(v["crashes"][0]["summary"], "module"), "workbench-worker.exe");
    bool record = false;
    for (const auto& r : v["records"])
        if (jstr(r, "event") == "crash.report" && jstr(r["attrs"], "crash.id") == id) record = true;
    EXPECT_TRUE(record);

    // Held for the user's review: not sent within the hour, sent after it.
    (void)rig.svc->flush();
    EXPECT_TRUE(rig.uploads.empty());
    rig.clock.advance(std::chrono::milliseconds(kCrashHoldMs + 1000));
    (void)rig.svc->flush();
    ASSERT_EQ(rig.uploads.size(), 1u);
    EXPECT_EQ(jstr(rig.uploads[0].first, "id"), id);
    EXPECT_EQ(jstr(rig.uploads[0].first, "exception_code"), "0xC000001D");
    EXPECT_EQ(jstr(rig.uploads[0].first, "installation_id"), "inst-1");
    EXPECT_GT(rig.uploads[0].second, 0u);
    EXPECT_EQ(jstr(rig.svc->view()["crashes"][0], "state"), "sent");

    // Another crash, deleted by the user before it is sent.
    {
        WorkerHost host(rig.worker_options());
        (void)host.call(WorkerRole::index, "crash", json::object());
    }
    ASSERT_EQ(rig.crashes.size(), 2u);
    rig.svc->capture_crash(rig.crashes[1].first, rig.crashes[1].second);
    v = rig.svc->view();
    ASSERT_EQ(v["crashes"].size(), 2u);
    const std::string second = jstr(v["crashes"][0], "id");
    ASSERT_TRUE(rig.svc->delete_crash(second));
    rig.clock.advance(std::chrono::milliseconds(kCrashHoldMs + 1000));
    (void)rig.svc->flush();
    EXPECT_EQ(rig.uploads.size(), 1u);
    EXPECT_EQ(rig.svc->view()["crashes"].size(), 1u);
    EXPECT_EQ(rig.files(), 0u);  // no dump file left behind
}

TEST(CrashReports, AnExtractionWorkerCrashIsCaptured) {
    WB_REQUIRES_DEV_ENDPOINTS();
    CrashRig rig;
    sandbox::EgressProxy proxy{[](const std::string&) { return policy::RuleVerdict{false, "test", "no network in tests"}; },
                               [](const sandbox::EgressProxy::Owner&, const std::string&, const policy::RuleVerdict&) {}};
    sandbox::Sandbox box{&proxy};
    tools::SandboxExtractorOptions o;
    o.worker_exe = exe_dir() / L"workbench-worker.exe";
    o.sandbox = &box;
    o.crash_dir = [&rig]() -> std::optional<std::filesystem::path> { return rig.dumps; };
    o.on_crash = [&rig](const std::filesystem::path& d, const std::string& p) { rig.crashes.emplace_back(d, p); };
    runtime::ToolContext ctx;
    auto r = tools::sandbox_extractor(o)("test-crash", "x", ctx);
    ASSERT_FALSE(r);
    ASSERT_EQ(rig.crashes.size(), 1u);
    EXPECT_EQ(rig.crashes[0].second, "worker-extract");
    auto s = crash::summarize(rig.crashes[0].first);
    ASSERT_TRUE(s) << s.error().message;
    EXPECT_EQ(s->exception_code, "0xC000001D");
    // A document that extracts normally leaves no file.
    auto ok = tools::sandbox_extractor(o)("docx", "not a docx", ctx);
    EXPECT_EQ(rig.crashes.size(), 1u);
    std::error_code ec;
    std::filesystem::remove(rig.crashes[0].first, ec);
    EXPECT_EQ(rig.files(), 0u);
}

TEST(CrashReports, NothingIsWrittenWhileCrashReportsAreDisabled) {
    CrashRig rig;
    rig.set_crash_reports(false);
    EXPECT_FALSE(rig.svc->crash_reports_enabled());
    {
        WorkerHost host(rig.worker_options());
        (void)host.call(WorkerRole::index, "crash", json::object());
    }
    EXPECT_TRUE(rig.crashes.empty());
    EXPECT_EQ(rig.files(), 0u);
    // A dump that reaches the service anyway (left from before the policy changed) is deleted, not kept.
    std::filesystem::create_directories(rig.dumps);
    const auto stale = rig.dumps / L"core-1.dmp";
    std::ofstream(stale) << "MDMP";
    rig.svc->import_pending(rig.dumps);
    EXPECT_FALSE(std::filesystem::exists(stale));
    EXPECT_TRUE(rig.svc->view()["crashes"].empty());
}