#include "tools/extractor.h"

#include "base/strings.h"
#include "platform/crash.h"
#include "platform/win.h"

namespace wb::tools {

Extractor sandbox_extractor(SandboxExtractorOptions o) {
    return [o](const std::string& kind, const std::string& bytes, runtime::ToolContext& ctx) -> Result<std::string> {
        if (!o.sandbox) return make_error("unavailable", "the tool sandbox is not running");
        for (const char c : kind)
            if (!((c >= 'a' && c <= 'z') || c == '-')) return make_error("unsupported", "unknown document kind");
        sandbox::ProcessSpec spec;
        spec.executable = o.worker_exe.wstring();
        spec.arguments = L"--role extract --kind " + widen(kind);
        spec.working_dir = narrow(o.worker_exe.parent_path().wstring());
        spec.privileges = sandbox::Privileges::restricted;
        spec.network = sandbox::Network::none;
        spec.limits.memory_mb = o.memory_mb;
        spec.limits.processes = 1;
        spec.limits.cpu_percent = 50;
        spec.limits.output_kb = 33 * 1024;  // the extractor's own text limit is 32 MB
        spec.timeout = o.timeout;
        spec.stdin_data = bytes;
        spec.run_id = ctx.run_id;
        spec.call_id = ctx.call_id;
        spec.tool = "extract";
        std::optional<crash::DumpSlot> dump;
        if (const auto dir = o.crash_dir ? o.crash_dir() : std::nullopt)
            if (auto slot = crash::open_dump_slot(*dir, "worker-extract")) {
                dump = std::move(*slot);
                spec.arguments += L" " + crash::dump_argument(*dump);
                spec.inherit_handles.push_back(dump->file.get());
            }
        auto r = o.sandbox->run(spec, [&ctx] { return ctx.cancelled(); });
        if (dump)
            if (auto written = crash::take_dump(*dump); written && o.on_crash) o.on_crash(*written, "worker-extract");
        if (!r) return make_error("extract_failed", "the extraction worker could not be started: " + r.error().message);
        switch (r->limit) {
            case sandbox::LimitHit::none: break;
            case sandbox::LimitHit::cancelled: return make_error("cancelled", "extraction was cancelled");
            case sandbox::LimitHit::time:
                return make_error("extract_failed", "the " + kind + " extractor did not finish within " +
                                                        std::to_string(o.timeout.count() / 1000) + " seconds and was stopped");
            default:
                return make_error("resource_limit", "the " + kind + " extractor exceeded its limits (" + std::string(to_string(r->limit)) +
                                                        ") and was stopped");
        }
        if (r->exit_code == static_cast<int>(0xE0000001))
            return make_error("resource_limit", "the " + kind + " extractor exceeded its memory budget of " + std::to_string(o.memory_mb) + " MB");
        if (r->exit_code != 0) {
            const std::string why = trim(r->err);
            if (!why.empty() && why.size() < 400 && why.find(": ") != std::string::npos)
                return make_error("extract_failed", why.substr(why.find(": ") + 2));
            return make_error("extract_failed", "the " + kind + " extractor failed (exit code 0x" + [&] {
                char b[16];
                std::snprintf(b, sizeof b, "%08X", static_cast<unsigned>(r->exit_code));
                return std::string(b);
            }() + ")");
        }
        if (r->out_truncated) return make_error("too_large", "the extracted text is larger than 32 MB");
        return r->out;
    };
}

}  // namespace wb::tools
