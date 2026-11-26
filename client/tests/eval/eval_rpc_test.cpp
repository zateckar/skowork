// The evaluation methods on the local API (client/evaluation Evaluation Setting; add-evaluation-export design D1).
#include <gtest/gtest.h>

#include <filesystem>

#include "app/eval_rpc.h"
#include "host/rpc.h"

using namespace wb;

namespace {

std::filesystem::path temp_dir(const std::string& name) {
    return std::filesystem::temp_directory_path() / ("skowork-eval-rpc-" + name);
}

}  // namespace

TEST(EvalRpc, OffMeansNotEnabled) {
    RpcRegistry rpc;
    register_eval_rpc(rpc, [] { return static_cast<eval::EvalService*>(nullptr); }, [] { return false; });
    const auto out = temp_dir("off");
    std::filesystem::remove_all(out);
    for (const char* method : {"eval.export", "eval.run", "eval.status", "eval.cancel"}) {
        const auto* h = rpc.find(method, true);
        ASSERT_NE(h, nullptr) << method;
        auto r = (*h)(json{{"out_dir", out.string()}, {"folder", out.string()}, {"job", "x"}, {"step", "triage"}});
        ASSERT_FALSE(r.ok()) << method;
        EXPECT_EQ(r.error().code, "not_enabled") << method;
    }
    EXPECT_FALSE(std::filesystem::exists(out));
}

TEST(EvalRpc, MethodsArePipeOnly) {
    RpcRegistry rpc;
    register_eval_rpc(rpc, [] { return static_cast<eval::EvalService*>(nullptr); }, [] { return true; });
    for (const char* method : {"eval.export", "eval.run", "eval.status", "eval.cancel"}) {
        EXPECT_EQ(rpc.find(method, false), nullptr) << method;
        EXPECT_NE(rpc.find(method, true), nullptr) << method;
    }
}

TEST(EvalRpc, OnWithoutServiceIsUnavailable) {
    RpcRegistry rpc;
    register_eval_rpc(rpc, [] { return static_cast<eval::EvalService*>(nullptr); }, [] { return true; });
    auto r = (*rpc.find("eval.status", true))(json{{"job", "x"}});
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code, "unavailable");
}
