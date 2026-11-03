// Generated from telemetry/catalogue.yaml by control-plane/scripts/telemetry-catalogue.ts. Do not edit.
// The telemetry allowlist (add-telemetry-and-admin design D1): only these fields can be emitted.
#pragma once

#include <cstdint>
#include <span>

namespace wb::telemetry {

enum class Level : std::uint8_t { off, metadata, content };
enum class Type : std::uint8_t { string, integer, number, boolean, domain };

struct FieldDef {
    const char* name;
    Type type;
    Level level;
};

enum class Res : std::uint8_t { service_version, workbench_channel, os_build, workbench_installation_id };
inline constexpr FieldDef kResource[] = {
    {"service.version", Type::string, Level::metadata},
    {"workbench.channel", Type::string, Level::metadata},
    {"os.build", Type::string, Level::metadata},
    {"workbench.installation_id", Type::string, Level::metadata},
};

enum class Attr : std::uint8_t {
    run_id,
    run_outcome,
    run_origin,
    run_duration_ms,
    run_tool_calls,
    tool_id,
    tool_risk,
    tool_outcome,
    tool_duration_ms,
    url_domain,
    approval_decision,
    policy_rule,
    policy_version,
    policy_outcome,
    model_role,
    tokens_prompt,
    tokens_completion,
    error_code,
    error_component,
    network_enforcement,
    process_startup_ms,
    process_working_set_mb,
    telemetry_discarded,
    crash_id,
    crash_process,
    crash_exception_code,
    crash_module,
    crash_dump_bytes,
    content_prompt,
    content_response,
    tool_arguments,
    error_message,
};
inline constexpr FieldDef kAttrs[] = {
    {"run.id", Type::string, Level::metadata},
    {"run.outcome", Type::string, Level::metadata},
    {"run.origin", Type::string, Level::metadata},
    {"run.duration_ms", Type::integer, Level::metadata},
    {"run.tool_calls", Type::integer, Level::metadata},
    {"tool.id", Type::string, Level::metadata},
    {"tool.risk", Type::string, Level::metadata},
    {"tool.outcome", Type::string, Level::metadata},
    {"tool.duration_ms", Type::integer, Level::metadata},
    {"url.domain", Type::domain, Level::metadata},
    {"approval.decision", Type::string, Level::metadata},
    {"policy.rule", Type::string, Level::metadata},
    {"policy.version", Type::integer, Level::metadata},
    {"policy.outcome", Type::string, Level::metadata},
    {"model.role", Type::string, Level::metadata},
    {"tokens.prompt", Type::integer, Level::metadata},
    {"tokens.completion", Type::integer, Level::metadata},
    {"error.code", Type::string, Level::metadata},
    {"error.component", Type::string, Level::metadata},
    {"network.enforcement", Type::string, Level::metadata},
    {"process.startup_ms", Type::integer, Level::metadata},
    {"process.working_set_mb", Type::number, Level::metadata},
    {"telemetry.discarded", Type::integer, Level::metadata},
    {"crash.id", Type::string, Level::metadata},
    {"crash.process", Type::string, Level::metadata},
    {"crash.exception_code", Type::string, Level::metadata},
    {"crash.module", Type::string, Level::metadata},
    {"crash.dump_bytes", Type::integer, Level::metadata},
    {"content.prompt", Type::string, Level::content},
    {"content.response", Type::string, Level::content},
    {"tool.arguments", Type::string, Level::content},
    {"error.message", Type::string, Level::content},
};

enum class Event : std::uint8_t {
    run_finished,
    tool_call,
    approval_resolved,
    policy_decision,
    model_usage,
    client_error,
    client_started,
    client_footprint,
    sandbox_enforcement,
    telemetry_buffer,
    crash_report,
};
namespace detail {
inline constexpr Attr k_run_finished[] = {Attr::run_id, Attr::run_outcome, Attr::run_origin, Attr::run_duration_ms, Attr::run_tool_calls, Attr::error_code, Attr::content_prompt, Attr::content_response};
inline constexpr Attr k_tool_call[] = {Attr::run_id, Attr::tool_id, Attr::tool_risk, Attr::tool_outcome, Attr::tool_duration_ms, Attr::url_domain, Attr::tool_arguments};
inline constexpr Attr k_approval_resolved[] = {Attr::run_id, Attr::tool_id, Attr::tool_risk, Attr::approval_decision};
inline constexpr Attr k_policy_decision[] = {Attr::run_id, Attr::tool_id, Attr::tool_risk, Attr::policy_rule, Attr::policy_version, Attr::policy_outcome};
inline constexpr Attr k_model_usage[] = {Attr::run_id, Attr::model_role, Attr::tokens_prompt, Attr::tokens_completion, Attr::error_code};
inline constexpr Attr k_client_error[] = {Attr::error_code, Attr::error_component, Attr::error_message};
inline constexpr Attr k_client_started[] = {Attr::process_startup_ms, Attr::network_enforcement};
inline constexpr Attr k_client_footprint[] = {Attr::process_working_set_mb};
inline constexpr Attr k_sandbox_enforcement[] = {Attr::network_enforcement};
inline constexpr Attr k_telemetry_buffer[] = {Attr::telemetry_discarded};
inline constexpr Attr k_crash_report[] = {Attr::crash_id, Attr::crash_process, Attr::crash_exception_code, Attr::crash_module, Attr::crash_dump_bytes};
}  // namespace detail
struct EventDef {
    const char* name;
    std::span<const Attr> attrs;
};
inline constexpr EventDef kEvents[] = {
    {"run.finished", detail::k_run_finished},
    {"tool.call", detail::k_tool_call},
    {"approval.resolved", detail::k_approval_resolved},
    {"policy.decision", detail::k_policy_decision},
    {"model.usage", detail::k_model_usage},
    {"client.error", detail::k_client_error},
    {"client.started", detail::k_client_started},
    {"client.footprint", detail::k_client_footprint},
    {"sandbox.enforcement", detail::k_sandbox_enforcement},
    {"telemetry.buffer", detail::k_telemetry_buffer},
    {"crash.report", detail::k_crash_report},
};

constexpr const FieldDef& def(Attr a) { return kAttrs[static_cast<int>(a)]; }
constexpr const FieldDef& def(Res r) { return kResource[static_cast<int>(r)]; }
constexpr const EventDef& def(Event e) { return kEvents[static_cast<int>(e)]; }

}  // namespace wb::telemetry
