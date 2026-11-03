// image.generate (add-image-generation task 3.2) in the agent runtime, with a scripted image generator in place of the
// gateway: the image is stored as a Thread attachment with the tool as provenance, the model is told only its id and
// size, and nothing is stored for data that is not an image, an image over the limit, a refusal or a cancelled Run.
#include "support/runtime_harness.h"
#include "tools/image.h"

#include <gtest/gtest.h>

using namespace wb;
using namespace wb::runtime;
using namespace wb::tools;

namespace {

std::vector<std::uint8_t> bytes_of(const std::string& s) { return {s.begin(), s.end()}; }

std::string be32(unsigned v) {
    return {static_cast<char>(v >> 24), static_cast<char>(v >> 16), static_cast<char>(v >> 8), static_cast<char>(v)};
}
// The start of a PNG file (signature and header chunk), enough for the type and size checks.
std::string png(unsigned w, unsigned h, size_t pad = 16) {
    return std::string("\x89PNG\r\n\x1a\n", 8) + be32(13) + "IHDR" + be32(w) + be32(h) + std::string("\x08\x06\x00\x00\x00", 5) +
           std::string(pad, '\x7f');
}

struct ImageRig {
    Env env;
    std::mutex mx;
    std::vector<std::pair<model::ImageRequest, model::RequestContext>> requests;
    std::function<model::ModelResult<std::vector<std::vector<std::uint8_t>>>(const model::RequestContext&)> answer;
    ImageToolEnv tool_env;

    ImageRig() {
        tool_env.generate = [this](const model::ImageRequest& r, const model::RequestContext& c) {
            {
                const std::lock_guard lock(mx);
                requests.emplace_back(r, c);
            }
            return answer(c);
        };
    }
    void start() {
        env.add_tool(image_generate_tool(tool_env));
        env.start();
    }
    // One Run whose model finds the tool (tools are loaded lazily), calls image.generate with `args`, then answers.
    std::string run(const json& args) {
        env.model.push_calls({{"f1", "find_tools", {{"query", "generate an image"}}}});
        env.model.push_calls({{"c1", "image.generate", args}});
        env.model.push_text("Here it is.");
        const auto t = env.thread();
        return env.run(t, "draw me a car");
    }
    json result(const std::string& run_id) {
        for (const auto& e : env.rt->events(run_id))
            if (e.kind == "tool_result" && jstr(e.payload, "call_id") == "c1") return e.payload;
        return nullptr;
    }
    long long attachments() {
        auto st = env.db->prepare("SELECT COUNT(*) FROM thread_attachments");
        return st.step() ? st.col_int(0) : -1;
    }
};

}  // namespace

TEST(ImageGenerate, StoresTheImageInTheThreadAndTellsTheModelOnlyItsIdAndSize) {
    ImageRig rig;
    rig.answer = [](const model::RequestContext&) { return std::vector<std::vector<std::uint8_t>>{bytes_of(png(1536, 1024))}; };
    rig.start();
    const auto run = rig.run({{"prompt", "a red car"}, {"size", "1536x1024"}});
    ASSERT_TRUE(rig.env.wait(run, RunState::succeeded));

    ASSERT_EQ(rig.requests.size(), 1U);
    EXPECT_EQ(rig.requests[0].first.prompt, "a red car");
    EXPECT_EQ(rig.requests[0].first.size, "1536x1024");
    EXPECT_EQ(rig.requests[0].first.count, 1);
    EXPECT_EQ(rig.requests[0].second.run_id, run);
    EXPECT_TRUE(rig.requests[0].second.cancel);  // the Run's cancellation reaches the request

    const json r = rig.result(run);
    ASSERT_TRUE(r.is_object());
    EXPECT_TRUE(jbool(r, "ok"));
    ASSERT_TRUE(r.contains("files"));
    ASSERT_EQ(r["files"].size(), 1U);
    const json& f = r["files"][0];
    EXPECT_EQ(jstr(f, "name"), "image-1.png");
    EXPECT_EQ(jstr(f, "mime"), "image/png");
    EXPECT_EQ(jint(f, "width"), 1536);
    EXPECT_EQ(jint(f, "height"), 1024);
    const std::string id = jstr(f, "id");
    const auto info = rig.env.rt->store().attachment(id);
    ASSERT_TRUE(info);
    EXPECT_EQ(info->run_id, run);
    EXPECT_EQ(info->call_id, "c1");
    EXPECT_EQ(info->size, static_cast<long long>(png(1536, 1024).size()));
    EXPECT_EQ(jstr(info->provenance, "source_kind"), "tool");
    EXPECT_EQ(jstr(info->provenance, "source_id"), "image.generate");
    auto stored = rig.env.rt->store().read_attachment(id, 0, info->size);
    ASSERT_TRUE(stored);
    EXPECT_EQ(*stored, png(1536, 1024));

    // The model gets the attachment id and the dimensions, never the bytes.
    const std::string content = jstr(r, "content");
    EXPECT_NE(content.find("Stored as attachment " + id), std::string::npos) << content;
    EXPECT_NE(content.find("1536x1024 px"), std::string::npos) << content;
    const auto reqs = rig.env.model.requests();
    ASSERT_EQ(reqs.size(), 3U);
    const std::string sent = reqs[2].messages.back().content;
    EXPECT_NE(sent.find(id), std::string::npos);
    EXPECT_EQ(sent.find("IHDR"), std::string::npos);
    EXPECT_TRUE(reqs[2].messages.back().images.empty());
}

TEST(ImageGenerate, DataThatIsNotAnImageIsNotStored) {
    ImageRig rig;
    rig.answer = [](const model::RequestContext&) {
        return std::vector<std::vector<std::uint8_t>>{bytes_of(png(64, 64)), bytes_of("<html>not an image</html>")};
    };
    rig.start();
    const auto run = rig.run({{"prompt", "two cars"}, {"count", 2}});
    ASSERT_TRUE(rig.env.wait(run, RunState::succeeded));
    const json r = rig.result(run);
    EXPECT_FALSE(jbool(r, "ok"));
    EXPECT_NE(jstr(r, "content").find("not a PNG, JPEG or WebP image"), std::string::npos);
    EXPECT_FALSE(r.contains("files"));
    EXPECT_EQ(rig.attachments(), 0);  // all or nothing: the valid first image is not kept either
}

TEST(ImageGenerate, AnImageOverTheLimitIsNotStored) {
    ImageRig rig;
    rig.tool_env.max_bytes = 100;
    rig.answer = [](const model::RequestContext&) { return std::vector<std::vector<std::uint8_t>>{bytes_of(png(64, 64, 200))}; };
    rig.start();
    const auto run = rig.run({{"prompt", "a big car"}});
    ASSERT_TRUE(rig.env.wait(run, RunState::succeeded));
    const json r = rig.result(run);
    EXPECT_FALSE(jbool(r, "ok"));
    EXPECT_NE(jstr(r, "content").find("more than the limit"), std::string::npos) << jstr(r, "content");
    EXPECT_EQ(rig.attachments(), 0);
}

TEST(ImageGenerate, ARefusalBecomesAnErrorResultTheAgentCanExplain) {
    ImageRig rig;
    rig.answer = [](const model::RequestContext&) -> model::ModelResult<std::vector<std::vector<std::uint8_t>>> {
        return failure(model::code::model_unavailable, "role not granted");
    };
    rig.start();
    const auto run = rig.run({{"prompt", "a car"}});
    ASSERT_TRUE(rig.env.wait(run, RunState::succeeded));
    const json r = rig.result(run);
    EXPECT_FALSE(jbool(r, "ok"));
    EXPECT_NE(jstr(r, "content").find("not available for this user"), std::string::npos) << jstr(r, "content");

    rig.answer = [](const model::RequestContext&) -> model::ModelResult<std::vector<std::vector<std::uint8_t>>> {
        model::ModelFailure f = failure(model::code::quota_exceeded, "quota");
        f.reset_at = "2026-10-25T00:00:00.000Z";
        return f;
    };
    const auto run2 = rig.run({{"prompt", "another car"}});
    ASSERT_TRUE(rig.env.wait(run2, RunState::succeeded));
    EXPECT_NE(jstr(rig.result(run2), "content").find("daily image budget is used up; it resets at 2026-10-25"), std::string::npos);
    EXPECT_EQ(rig.attachments(), 0);
}

TEST(ImageGenerate, StoppingTheRunWhileGeneratingStoresNothing) {
    ImageRig rig;
    std::atomic<bool> started{false};
    rig.answer = [&](const model::RequestContext& c) -> model::ModelResult<std::vector<std::vector<std::uint8_t>>> {
        started = true;
        const auto end = std::chrono::steady_clock::now() + 10s;
        while (!(c.cancel && c.cancel->cancelled()) && std::chrono::steady_clock::now() < end) std::this_thread::sleep_for(2ms);
        // The gateway's answer arrives anyway; the cancelled Run must not keep it.
        return std::vector<std::vector<std::uint8_t>>{bytes_of(png(64, 64))};
    };
    rig.start();
    const auto run = rig.run({{"prompt", "a slow car"}});
    ASSERT_TRUE(rig.env.wait_for([&] { return started.load(); }));
    ASSERT_TRUE(rig.env.rt->cancel(run));
    ASSERT_TRUE(rig.env.wait(run, RunState::cancelled));
    EXPECT_EQ(rig.attachments(), 0);
}

TEST(ImageGenerate, ArgumentsOutsideTheLimitsAreRefusedBeforeAnyRequest) {
    ImageRig rig;
    rig.answer = [](const model::RequestContext&) { return std::vector<std::vector<std::uint8_t>>{bytes_of(png(8, 8))}; };
    const ToolDef t = image_generate_tool(rig.tool_env);
    EXPECT_EQ(t.risk, Risk::read);
    ToolContext ctx;
    for (const json& args : {json{{"prompt", ""}}, json{{"prompt", std::string(4001, 'x')}}, json{{"prompt", "x"}, {"size", "512x512"}},
                             json{{"prompt", "x"}, {"count", 5}}, json{{"prompt", "x"}, {"count", 0}}}) {
        EXPECT_FALSE(t.run(ctx, args).ok) << args.dump();
    }
    EXPECT_TRUE(rig.requests.empty());
    const ToolOutput ok = t.run(ctx, {{"prompt", "x"}, {"count", 1}});
    EXPECT_TRUE(ok.ok);
    ASSERT_EQ(ok.files.size(), 1U);
    EXPECT_EQ(ok.files[0].width, 8);
}
