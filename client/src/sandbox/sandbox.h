// client/tool-sandbox (add-policy-and-sandbox design D5): every process the client starts on the agent's behalf (shell
// commands, skill scripts, stdio MCP servers) runs in its own Job Object with limits, with a restricted token and a
// scrubbed environment, with network only through the egress proxy (or none), and with bounded output capture.
#pragma once

#include "base/result.h"
#include "policy/core.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace wb::sandbox {

class EgressProxy;

// policy.tools.<group>.limits (Resource Limits, Output Capture Limits).
struct Limits {
    long long memory_mb = 1024;
    int cpu_percent = 50;
    int processes = 32;
    int wall_minutes = 15;
    long long output_kb = 1024;  // per stream
};
// The limits of a tool group (shell, skills, mcp...) from the effective policy; defaults where unset.
Limits limits_for(const policy::EffectivePolicy* policy, const std::string& group);

// policy.tools.shell.mode (Shell Modes).
enum class ShellMode { off, sandboxed, allow };
ShellMode shell_mode(const policy::EffectivePolicy* policy);

enum class Privileges {
    restricted,  // restricted token (no admin group, minimal privileges) and a scrubbed environment
    user,        // the user's normal token (shell mode allow); still contained, still a scrubbed environment
};

enum class Network {
    proxy,   // HTTP(S)_PROXY to the egress proxy with a per-process credential
    none,    // the egress proxy with a credential that every destination is refused for (no OS network isolation)
    direct,  // no proxy variables (shell mode allow)
};

struct ProcessSpec {
    std::wstring executable;  // full path
    std::wstring arguments;   // the command line after the executable, already quoted
    std::string working_dir;  // checked by the caller against the filesystem rules
    Privileges privileges = Privileges::restricted;
    Network network = Network::proxy;
    Limits limits;
    // A per-call timeout: it can only shorten limits.wall_minutes.
    std::optional<std::chrono::milliseconds> timeout;
    std::vector<std::pair<std::string, std::string>> env;  // added to the scrubbed environment
    std::string stdin_data;
    // Further handles the process inherits (a crash dump file); they must be inheritable.
    std::vector<void*> inherit_handles;
    // For egress denials in the approval log.
    std::string run_id, call_id, tool;
};

enum class LimitHit { none, memory, processes, time, cancelled };
const char* to_string(LimitHit l);

struct ProcessResult {
    int exit_code = 0;
    LimitHit limit = LimitHit::none;
    std::string out, err;  // UTF-8
    bool out_truncated = false, err_truncated = false;
    long long out_bytes = 0, err_bytes = 0;  // everything the process wrote
    long long duration_ms = 0;
};

// A readable sentence for a limit-exceeded tool result.
std::string describe_limit(LimitHit l, const Limits& limits, std::chrono::milliseconds wall);

// The wall-clock limit of a spec: the smaller of limits.wall_minutes and the per-call timeout.
std::chrono::milliseconds effective_wall(const ProcessSpec& spec);

// The environment a sandboxed process starts with: an allowlist of the client's variables plus `extra`. Nothing else
// is inherited, so no Skowork token or secret reaches the child.
std::vector<std::pair<std::string, std::string>> scrubbed_environment(const std::vector<std::pair<std::string, std::string>>& extra);

// Decodes captured console output to UTF-8: UTF-8 as is, UTF-16LE when it looks like it, otherwise the OEM code page.
// A sequence cut by truncation is dropped.
std::string decode_output(const std::string& bytes);

// The process settings for a shell command under policy.tools.shell.mode (Shell Modes): error "tool_unavailable" when
// the mode is off; in sandboxed mode the working folder must be writable under the filesystem rules (otherwise
// "policy_denied"), and the process gets the restricted token and the egress proxy; in allow mode the user's token and
// direct network. Limits come from policy.tools.shell.limits. The caller fills in the executable and arguments.
// `final_working_dir` is the canonical path of the folder (policy::canonicalize).
Result<ProcessSpec> shell_process(const policy::EffectivePolicy* policy, const policy::PathRules& rules, const std::string& final_working_dir);

struct Launched;

// A long-lived sandboxed process (a stdio MCP server): the same job, token, environment and network as Sandbox::run,
// without the wall-clock limit. The caller talks to it over stdin/stdout; stderr is kept as a bounded tail. Memory and
// process-count limits terminate the job. Destruction terminates every process of the job.
class SandboxedProcess {
public:
    ~SandboxedProcess();
    SandboxedProcess(const SandboxedProcess&) = delete;
    SandboxedProcess& operator=(const SandboxedProcess&) = delete;

    // Writes to the process's stdin (thread safe). Fails when the pipe is closed.
    Status write(std::string_view data);
    // Blocks until stdout has data; returns an empty string at end of stream (the process ended or was terminated).
    std::string read_some();
    [[nodiscard]] bool running() const;
    // The exit code once the process ended; -1 while it runs.
    [[nodiscard]] int exit_code() const;
    // The limit that terminated the job, if any.
    [[nodiscard]] LimitHit limit() const { return limit_.load(); }
    // The last bytes the process wrote to stderr (at most 8 KB), UTF-8.
    [[nodiscard]] std::string stderr_tail() const;
    [[nodiscard]] unsigned long pid() const;
    // Terminates every process of the job; read_some then returns end of stream. Idempotent.
    void terminate();
    // Waits until the process has ended and its stderr is fully read (for complete diagnostics after a crash).
    bool wait_ended(std::chrono::milliseconds timeout);

private:
    friend class Sandbox;
    explicit SandboxedProcess(std::unique_ptr<Launched> l);
    void monitor();

    std::unique_ptr<Launched> l_;
    std::atomic<LimitHit> limit_{LimitHit::none};
    std::atomic<bool> stopping_{false};
    std::atomic<bool> err_done_{false};
    mutable std::mutex mx_;
    std::string err_tail_;
    std::thread err_reader_, monitor_;
};

class Sandbox {
public:
    explicit Sandbox(EgressProxy* proxy = nullptr) : proxy_(proxy) {}

    // Starts the process suspended, assigns it to a fresh job with the limits, resumes it and waits until it exits, a
    // limit is hit or `cancelled` returns true. Every process of the job is terminated before this returns.
    Result<ProcessResult> run(const ProcessSpec& spec, const std::function<bool()>& cancelled = {});

    // Starts a long-lived process (spec.timeout, limits.wall_minutes and stdin_data are ignored).
    Result<std::unique_ptr<SandboxedProcess>> start(const ProcessSpec& spec);

private:
    Result<std::unique_ptr<Launched>> launch(const ProcessSpec& spec);
    EgressProxy* proxy_;
};

}  // namespace wb::sandbox
