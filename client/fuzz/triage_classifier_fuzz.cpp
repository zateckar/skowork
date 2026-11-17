#include "fuzz_main.h"
#include "triage/classifier.h"

namespace {
class FuzzModel : public wb::model::IChatModel {
public:
    std::string output;
    std::optional<wb::model::RoleInfo> role_info(wb::model::Role role) override {
        wb::model::RoleInfo info;
        info.role = role;
        info.kind = "chat";
        return info;
    }
    wb::model::ModelResult<wb::model::ChatResponse> chat(const wb::model::ChatRequest&, const wb::model::ChatSink&) override {
        wb::model::ChatResponse response;
        response.finish_reason = "stop";
        response.text = output;
        return response;
    }
};
}

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    FuzzModel model;
    model.output.assign(reinterpret_cast<const char*>(data), size);
    const auto result = wb::triage::classify(model, "review budgets", {{"event", "revision", "source"}});
    if (result) {
        if (result->size() != 1 || (*result)[0].id != "event" || (*result)[0].revision != "revision" ||
            ((*result)[0].outcome != "relevant" && (*result)[0].outcome != "not_relevant" && (*result)[0].outcome != "uncertain") ||
            (*result)[0].reason.size() > 512) __builtin_trap();
    }
    return 0;
}
