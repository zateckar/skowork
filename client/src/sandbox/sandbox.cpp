#include "sandbox/sandbox.h"

#include "base/strings.h"
#include "platform/log.h"
#include "platform/win.h"
#include "sandbox/proxy.h"

#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>
#include <thread>

namespace wb::sandbox {

namespace {

constexpr const char* kLog = "sandbox";
constexpr ULONG_PTR kJobKey = 1;

// ---- Policy values --------------------------------------------------------------------------------------------------

long long limit_value(const policy::EffectivePolicy* p, const std::string& group, const char* name, long long fallback) {
    if (!p) return fallback;
    return p->integer("tools." + group + ".limits." + name, fallback);
}

// ---- Token ----------------------------------------------------------------------------------------------------------

// The client's token without administrative group membership and with no privilege beyond SeChangeNotifyPrivilege.
Result<unique_handle> restricted_token() {
    HANDLE raw = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(),
                            TOKEN_DUPLICATE | TOKEN_ASSIGN_PRIMARY | TOKEN_QUERY | TOKEN_ADJUST_DEFAULT | TOKEN_ADJUST_SESSIONID, &raw))
        return make_error("sandbox", "OpenProcessToken: " + error_text(::GetLastError()));
    unique_handle token(raw);
    BYTE admins[SECURITY_MAX_SID_SIZE];
    DWORD size = sizeof admins;
    if (!::CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, admins, &size))
        return make_error("sandbox", "CreateWellKnownSid: " + error_text(::GetLastError()));
    SID_AND_ATTRIBUTES disable{admins, 0};
    HANDLE restricted = nullptr;
    if (!::CreateRestrictedToken(token.get(), DISABLE_MAX_PRIVILEGE, 1, &disable, 0, nullptr, 0, nullptr, &restricted))
        return make_error("sandbox", "CreateRestrictedToken: " + error_text(::GetLastError()));
    return unique_handle(restricted);
}

// ---- Environment ----------------------------------------------------------------------------------------------------

constexpr const wchar_t* kInherited[] = {
    L"ALLUSERSPROFILE", L"APPDATA", L"COMPUTERNAME", L"ComSpec", L"CommonProgramFiles", L"CommonProgramFiles(x86)",
    L"CommonProgramW6432", L"HOMEDRIVE", L"HOMEPATH", L"LOCALAPPDATA", L"NUMBER_OF_PROCESSORS", L"OS", L"PATH", L"PATHEXT",
    L"PROCESSOR_ARCHITECTURE", L"PROCESSOR_IDENTIFIER", L"PROCESSOR_LEVEL", L"PROCESSOR_REVISION", L"ProgramData", L"ProgramFiles",
    L"ProgramFiles(x86)", L"ProgramW6432", L"PSModulePath", L"PUBLIC", L"SystemDrive", L"SystemRoot", L"TEMP", L"TMP", L"USERDOMAIN",
    L"USERNAME", L"USERPROFILE", L"windir"};

std::wstring environment_block(const std::vector<std::pair<std::string, std::string>>& vars) {
    std::vector<std::wstring> entries;
    entries.reserve(vars.size());
    for (const auto& [k, v] : vars) entries.push_back(widen(k) + L"=" + widen(v));
    // Windows expects the block sorted by name, case-insensitively.
    std::sort(entries.begin(), entries.end(), [](const std::wstring& a, const std::wstring& b) {
        const auto na = a.substr(0, a.find(L'=')), nb = b.substr(0, b.find(L'='));
        return _wcsicmp(na.c_str(), nb.c_str()) < 0;
    });
    std::wstring block;
    for (const auto& e : entries) {
        block += e;
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    return block;
}

void set_var(std::vector<std::pair<std::string, std::string>>& vars, const std::string& name, const std::string& value) {
    for (auto& [k, v] : vars)
        if (iequals(k, name)) {
            v = value;
            return;
        }
    vars.emplace_back(name, value);
}

// ---- Output capture -------------------------------------------------------------------------------------------------

struct Capture {
    std::string data;
    long long total = 0;
    bool truncated = false;
};

void read_stream(HANDLE h, size_t limit, Capture& c) {
    char buf[64 * 1024];
    for (;;) {
        DWORD n = 0;
        if (!::ReadFile(h, buf, sizeof buf, &n, nullptr) || n == 0) return;  // broken pipe: every writer has exited
        c.total += n;
        if (c.data.size() < limit) {
            const size_t take = std::min<size_t>(n, limit - c.data.size());
            c.data.append(buf, take);
            if (take < n) c.truncated = true;
        } else {
            c.truncated = true;  // keep draining so that the process is never blocked on a full pipe
        }
    }
}

bool valid_utf8(std::string_view s) {
    for (size_t i = 0; i < s.size();) {
        const auto c = static_cast<unsigned char>(s[i]);
        size_t n = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 0;
        if (n == 0 || i + n > s.size()) return false;
        for (size_t k = 1; k < n; ++k)
            if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) return false;
        i += n;
    }
    return true;
}

std::string from_codepage(UINT cp, std::string_view s) {
    if (s.empty()) return {};
    const int n = ::MultiByteToWideChar(cp, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(static_cast<size_t>(n), L'\0');
    ::MultiByteToWideChar(cp, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return narrow(w);
}

}  // namespace

// ---- Public helpers -------------------------------------------------------------------------------------------------

Limits limits_for(const policy::EffectivePolicy* p, const std::string& group) {
    Limits l;
    l.memory_mb = std::max<long long>(16, limit_value(p, group, "memory_mb", l.memory_mb));
    l.cpu_percent = static_cast<int>(std::clamp<long long>(limit_value(p, group, "cpu_percent", l.cpu_percent), 1, 100));
    l.processes = static_cast<int>(std::clamp<long long>(limit_value(p, group, "processes", l.processes), 1, 4096));
    l.wall_minutes = static_cast<int>(std::max<long long>(1, limit_value(p, group, "wall_minutes", l.wall_minutes)));
    l.output_kb = std::max<long long>(1, limit_value(p, group, "output_kb", l.output_kb));
    return l;
}

ShellMode shell_mode(const policy::EffectivePolicy* p) {
    const std::string m = p ? p->string("tools.shell.mode", "off") : "off";
    return m == "allow" ? ShellMode::allow : m == "sandboxed" ? ShellMode::sandboxed : ShellMode::off;
}

Result<ProcessSpec> shell_process(const policy::EffectivePolicy* p, const policy::PathRules& rules, const std::string& final_working_dir) {
    const ShellMode mode = shell_mode(p);
    if (mode == ShellMode::off) return make_error("tool_unavailable", "the shell is switched off by policy (policy.tools.shell.mode)");
    ProcessSpec s;
    s.limits = limits_for(p, "shell");
    s.working_dir = final_working_dir;
    if (mode == ShellMode::allow) {
        s.privileges = Privileges::user;
        s.network = Network::direct;
        return s;
    }
    const policy::RuleVerdict v = policy::check_path(final_working_dir, policy::Access::write, rules);
    if (!v.allowed)
        return make_error("policy_denied", "the working folder " + final_working_dir + " is not writable under the policy (" + v.rule + ")");
    s.privileges = Privileges::restricted;
    s.network = Network::proxy;
    return s;
}

const char* to_string(LimitHit l) {
    switch (l) {
        case LimitHit::none: return "none";
        case LimitHit::memory: return "memory";
        case LimitHit::processes: return "processes";
        case LimitHit::time: return "time";
        case LimitHit::cancelled: return "cancelled";
    }
    return "none";
}

std::string describe_limit(LimitHit l, const Limits& limits, std::chrono::milliseconds wall) {
    switch (l) {
        case LimitHit::memory: return "The command was stopped: it exceeded the memory limit of " + std::to_string(limits.memory_mb) + " MB.";
        case LimitHit::processes:
            return "The command was stopped: it exceeded the limit of " + std::to_string(limits.processes) + " processes.";
        case LimitHit::time: {
            const long long s = wall.count() / 1000;
            return "The command was stopped: it exceeded the time limit of " +
                   (s % 60 == 0 ? std::to_string(s / 60) + " minutes." : std::to_string(s) + " seconds.");
        }
        case LimitHit::cancelled: return "The command was stopped because the Run was cancelled.";
        case LimitHit::none: break;
    }
    return {};
}

std::chrono::milliseconds effective_wall(const ProcessSpec& spec) {
    const std::chrono::milliseconds policy_wall = std::chrono::minutes(spec.limits.wall_minutes);
    if (spec.timeout && *spec.timeout > std::chrono::milliseconds(0)) return std::min(policy_wall, *spec.timeout);
    return policy_wall;
}

std::vector<std::pair<std::string, std::string>> scrubbed_environment(const std::vector<std::pair<std::string, std::string>>& extra) {
    std::vector<std::pair<std::string, std::string>> vars;
    for (const wchar_t* name : kInherited) {
        const DWORD n = ::GetEnvironmentVariableW(name, nullptr, 0);
        if (n == 0) continue;
        std::wstring v(n, L'\0');
        const DWORD m = ::GetEnvironmentVariableW(name, v.data(), n);
        v.resize(m);
        vars.emplace_back(narrow(name), narrow(v));
    }
    for (const auto& [k, v] : extra) set_var(vars, k, v);
    return vars;
}

std::string decode_output(const std::string& bytes) {
    if (bytes.empty()) return {};
    // UTF-16LE: a byte-order mark, or many zero bytes in odd positions (ASCII text written as UTF-16).
    size_t odd_zeros = 0;
    for (size_t i = 1; i < bytes.size(); i += 2)
        if (bytes[i] == '\0') ++odd_zeros;
    const bool bom16 = bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xFF && static_cast<unsigned char>(bytes[1]) == 0xFE;
    if (bom16 || (bytes.size() >= 4 && odd_zeros * 4 >= bytes.size())) {
        std::wstring w(bytes.size() / 2, L'\0');
        std::memcpy(w.data(), bytes.data(), w.size() * 2);
        if (bom16) w.erase(0, 1);
        if (!w.empty() && w.back() >= 0xD800 && w.back() <= 0xDBFF) w.pop_back();  // a surrogate cut by truncation
        return narrow(w);
    }
    // UTF-8, possibly with a sequence cut at the capture limit: drop an incomplete trailing sequence first.
    std::string_view t = bytes;
    for (size_t back = 1; back <= 3 && back <= t.size(); ++back) {
        const auto c = static_cast<unsigned char>(t[t.size() - back]);
        if ((c & 0xC0) == 0x80) continue;  // continuation byte
        const size_t need = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
        if (need > back) t.remove_suffix(back);
        break;
    }
    if (valid_utf8(t)) return std::string(t);
    return from_codepage(::GetOEMCP(), bytes);
}

// ---- Launch (shared by run and start) ------------------------------------------------------------------------------

struct Launched {
    unique_handle job, port, process, thread;
    unique_handle parent_in, parent_out, parent_err;
    std::unique_ptr<EgressProxy::Session> session;
    std::chrono::steady_clock::time_point started;
    std::mutex write_mx;
};

Result<std::unique_ptr<Launched>> Sandbox::launch(const ProcessSpec& spec) {
    if (spec.executable.empty()) return make_error("invalid_params", "no executable");
    const std::wstring wd = widen(spec.working_dir);
    if (!wd.empty() && !(::GetFileAttributesW(wd.c_str()) & FILE_ATTRIBUTE_DIRECTORY))
        return make_error("invalid_params", "the working folder does not exist: " + spec.working_dir);
    if (spec.network != Network::direct && !proxy_) return make_error("sandbox", "the egress proxy is unavailable");
    auto l = std::make_unique<Launched>();

    // 1. The job: kill-on-close, no breakaway, memory, process count and CPU rate (design D5).
    unique_handle job(::CreateJobObjectW(nullptr, nullptr));
    if (!job) return make_error("sandbox", "CreateJobObject: " + error_text(::GetLastError()));
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
    li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_JOB_MEMORY | JOB_OBJECT_LIMIT_ACTIVE_PROCESS |
                                          JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
    li.BasicLimitInformation.ActiveProcessLimit = static_cast<DWORD>(spec.limits.processes);
    li.JobMemoryLimit = static_cast<SIZE_T>(spec.limits.memory_mb) * 1024 * 1024;
    if (!::SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &li, sizeof li))
        return make_error("sandbox", "job limits: " + error_text(::GetLastError()));
    JOBOBJECT_CPU_RATE_CONTROL_INFORMATION cpu{};
    cpu.ControlFlags = JOB_OBJECT_CPU_RATE_CONTROL_ENABLE | JOB_OBJECT_CPU_RATE_CONTROL_HARD_CAP;
    cpu.CpuRate = static_cast<DWORD>(spec.limits.cpu_percent) * 100;
    if (!::SetInformationJobObject(job.get(), JobObjectCpuRateControlInformation, &cpu, sizeof cpu))
        return make_error("sandbox", "job CPU rate: " + error_text(::GetLastError()));
    unique_handle port(::CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1));
    JOBOBJECT_ASSOCIATE_COMPLETION_PORT acp{};
    acp.CompletionKey = reinterpret_cast<PVOID>(kJobKey);
    acp.CompletionPort = port.get();
    if (!port || !::SetInformationJobObject(job.get(), JobObjectAssociateCompletionPortInformation, &acp, sizeof acp))
        return make_error("sandbox", "job port: " + error_text(::GetLastError()));

    // 2. Environment and proxy credential; in no-network mode the proxy refuses every destination for it.
    std::vector<std::pair<std::string, std::string>> extra = spec.env;
    std::unique_ptr<EgressProxy::Session> session;
    if (spec.network != Network::direct) {
        auto s = proxy_->open({spec.run_id, spec.call_id, spec.tool, spec.network == Network::none});
        if (!s) return s.error();
        session = std::move(*s);
        const std::string url = session->proxy_url();
        for (const char* k : {"HTTP_PROXY", "HTTPS_PROXY", "ALL_PROXY", "http_proxy", "https_proxy", "all_proxy"}) extra.emplace_back(k, url);
        extra.emplace_back("NO_PROXY", "");
        extra.emplace_back("no_proxy", "");
    }
    const std::wstring env = environment_block(scrubbed_environment(extra));

    // 3. Pipes; only the child's ends are inheritable, and only they are passed (handle list).
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE in_r = nullptr, in_w = nullptr, out_r = nullptr, out_w = nullptr, err_r = nullptr, err_w = nullptr;
    if (!::CreatePipe(&in_r, &in_w, &sa, 0)) return make_error("sandbox", "CreatePipe failed");
    unique_handle child_in(in_r), parent_in(in_w);
    if (!::CreatePipe(&out_r, &out_w, &sa, 0)) return make_error("sandbox", "CreatePipe failed");
    unique_handle parent_out(out_r), child_out(out_w);
    if (!::CreatePipe(&err_r, &err_w, &sa, 0)) return make_error("sandbox", "CreatePipe failed");
    unique_handle parent_err(err_r), child_err(err_w);
    for (HANDLE h : {in_w, out_r, err_r}) ::SetHandleInformation(h, HANDLE_FLAG_INHERIT, 0);

    const DWORD attr_count = 1;
    SIZE_T attr_size = 0;
    ::InitializeProcThreadAttributeList(nullptr, attr_count, 0, &attr_size);
    std::vector<unsigned char> attr_buf(attr_size);
    auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_buf.data());
    if (!::InitializeProcThreadAttributeList(attrs, attr_count, 0, &attr_size)) return make_error("sandbox", "attribute list");
    std::vector<HANDLE> inherit{in_r, out_w, err_w};
    for (void* h : spec.inherit_handles) inherit.push_back(static_cast<HANDLE>(h));
    ::UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit.data(), inherit.size() * sizeof(HANDLE), nullptr, nullptr);
    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof si;
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = in_r;
    si.StartupInfo.hStdOutput = out_w;
    si.StartupInfo.hStdError = err_w;
    si.lpAttributeList = attrs;

    // 4. Create suspended, assign to the job, resume.
    std::wstring cmd = L"\"" + spec.executable + L"\"";
    if (!spec.arguments.empty()) cmd += L" " + spec.arguments;
    const DWORD flags = CREATE_SUSPENDED | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT;
    PROCESS_INFORMATION pi{};
    BOOL ok = FALSE;
    const auto started = std::chrono::steady_clock::now();
    if (spec.privileges == Privileges::restricted) {
        auto token = restricted_token();
        if (!token) {
            ::DeleteProcThreadAttributeList(attrs);
            return token.error();
        }
        ok = ::CreateProcessAsUserW(token->get(), spec.executable.c_str(), cmd.data(), nullptr, nullptr, TRUE, flags,
                                    const_cast<wchar_t*>(env.c_str()), wd.empty() ? nullptr : wd.c_str(), &si.StartupInfo, &pi);
    } else {
        ok = ::CreateProcessW(spec.executable.c_str(), cmd.data(), nullptr, nullptr, TRUE, flags, const_cast<wchar_t*>(env.c_str()),
                              wd.empty() ? nullptr : wd.c_str(), &si.StartupInfo, &pi);
    }
    const DWORD create_error = ::GetLastError();
    ::DeleteProcThreadAttributeList(attrs);
    if (!ok) return make_error("sandbox", "cannot start " + narrow(spec.executable) + ": " + error_text(create_error));
    unique_handle process(pi.hProcess), thread(pi.hThread);
    if (!::AssignProcessToJobObject(job.get(), pi.hProcess)) {
        const DWORD e = ::GetLastError();
        ::TerminateProcess(pi.hProcess, 1);
        return make_error("sandbox", "AssignProcessToJobObject: " + error_text(e));
    }
    ::ResumeThread(pi.hThread);
    child_in.reset();
    child_out.reset();
    child_err.reset();
    l->job = std::move(job);
    l->port = std::move(port);
    l->process = std::move(process);
    l->thread = std::move(thread);
    l->parent_in = std::move(parent_in);
    l->parent_out = std::move(parent_out);
    l->parent_err = std::move(parent_err);
    l->session = std::move(session);
    l->started = started;
    return l;
}

// ---- Sandbox::run ---------------------------------------------------------------------------------------------------

Result<ProcessResult> Sandbox::run(const ProcessSpec& spec, const std::function<bool()>& cancelled) {
    auto launched = launch(spec);
    if (!launched) return launched.error();
    Launched& l = **launched;
    const auto wall = effective_wall(spec);
    const size_t out_limit = static_cast<size_t>(spec.limits.output_kb) * 1024;
    auto& job = l.job;
    auto& port = l.port;
    auto& process = l.process;
    auto& parent_in = l.parent_in;
    auto& parent_out = l.parent_out;
    auto& parent_err = l.parent_err;
    const auto started = l.started;

    // 5. Capture and feed.
    Capture out, err;
    std::thread out_reader([&] { read_stream(parent_out.get(), out_limit, out); });
    std::thread err_reader([&] { read_stream(parent_err.get(), out_limit, err); });
    std::thread writer([&] {
        size_t at = 0;
        while (at < spec.stdin_data.size()) {
            DWORD n = 0;
            const DWORD chunk = static_cast<DWORD>(std::min<size_t>(spec.stdin_data.size() - at, 64 * 1024));
            if (!::WriteFile(parent_in.get(), spec.stdin_data.data() + at, chunk, &n, nullptr)) break;
            at += n;
        }
        parent_in.reset();
    });

    // 6. Wait for the process, a limit or cancellation.
    ProcessResult r;
    const auto deadline = started + wall;
    for (;;) {
        if (::WaitForSingleObject(process.get(), 0) == WAIT_OBJECT_0) break;
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            r.limit = LimitHit::time;
            break;
        }
        if (cancelled && cancelled()) {
            r.limit = LimitHit::cancelled;
            break;
        }
        const auto wait = std::min<long long>(100, std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count() + 1);
        DWORD msg = 0;
        ULONG_PTR key = 0;
        LPOVERLAPPED ov = nullptr;
        if (::GetQueuedCompletionStatus(port.get(), &msg, &key, &ov, static_cast<DWORD>(wait)) && key == kJobKey) {
            if (msg == JOB_OBJECT_MSG_JOB_MEMORY_LIMIT || msg == JOB_OBJECT_MSG_PROCESS_MEMORY_LIMIT) {
                r.limit = LimitHit::memory;
                break;
            }
            if (msg == JOB_OBJECT_MSG_ACTIVE_PROCESS_LIMIT) {
                r.limit = LimitHit::processes;
                break;
            }
            if (msg == JOB_OBJECT_MSG_ACTIVE_PROCESS_ZERO) break;
        }
    }
    // Every process of the group ends here, including anything the command left running.
    ::TerminateJobObject(job.get(), r.limit == LimitHit::none ? 0 : 1);
    ::WaitForSingleObject(process.get(), 10000);
    DWORD code = 0;
    ::GetExitCodeProcess(process.get(), &code);
    r.exit_code = static_cast<int>(code);
    writer.join();
    out_reader.join();
    err_reader.join();
    r.duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
    r.out = decode_output(out.data);
    r.err = decode_output(err.data);
    r.out_truncated = out.truncated;
    r.err_truncated = err.truncated;
    r.out_bytes = out.total;
    r.err_bytes = err.total;
    if (r.limit != LimitHit::none)
        log::info(kLog, std::string("process stopped: ") + to_string(r.limit) + " limit (" + spec.tool + ")");
    return r;
}

// ---- Sandbox::start and SandboxedProcess ----------------------------------------------------------------------------

Result<std::unique_ptr<SandboxedProcess>> Sandbox::start(const ProcessSpec& spec) {
    auto launched = launch(spec);
    if (!launched) return launched.error();
    return std::unique_ptr<SandboxedProcess>(new SandboxedProcess(std::move(*launched)));
}

SandboxedProcess::SandboxedProcess(std::unique_ptr<Launched> l) : l_(std::move(l)) {
    err_reader_ = std::thread([this] {
        constexpr size_t kTail = 8 * 1024;
        char buf[4096];
        for (;;) {
            DWORD n = 0;
            if (!::ReadFile(l_->parent_err.get(), buf, sizeof buf, &n, nullptr) || n == 0) {
                err_done_.store(true);
                return;
            }
            const std::lock_guard lock(mx_);
            err_tail_.append(buf, n);
            if (err_tail_.size() > kTail) err_tail_.erase(0, err_tail_.size() - kTail);
        }
    });
    monitor_ = std::thread([this] { monitor(); });
}

void SandboxedProcess::monitor() {
    while (!stopping_.load()) {
        DWORD msg = 0;
        ULONG_PTR key = 0;
        LPOVERLAPPED ov = nullptr;
        if (!::GetQueuedCompletionStatus(l_->port.get(), &msg, &key, &ov, 200) || key != kJobKey) continue;
        LimitHit hit = LimitHit::none;
        if (msg == JOB_OBJECT_MSG_JOB_MEMORY_LIMIT || msg == JOB_OBJECT_MSG_PROCESS_MEMORY_LIMIT) hit = LimitHit::memory;
        else if (msg == JOB_OBJECT_MSG_ACTIVE_PROCESS_LIMIT) hit = LimitHit::processes;
        else if (msg == JOB_OBJECT_MSG_ACTIVE_PROCESS_ZERO) {
            ::TerminateJobObject(l_->job.get(), 0);  // close the pipes of anything left
            return;
        }
        if (hit != LimitHit::none) {
            limit_.store(hit);
            log::info(kLog, std::string("long-lived process stopped: ") + to_string(hit) + " limit");
            ::TerminateJobObject(l_->job.get(), 1);
            return;
        }
    }
}

SandboxedProcess::~SandboxedProcess() {
    terminate();
    stopping_.store(true);
    {
        const std::lock_guard lock(l_->write_mx);
        l_->parent_in.reset();
    }
    if (monitor_.joinable()) monitor_.join();
    if (err_reader_.joinable()) err_reader_.join();
}

Status SandboxedProcess::write(std::string_view data) {
    const std::lock_guard lock(l_->write_mx);
    if (!l_->parent_in) return make_error("closed", "the process input is closed");
    size_t at = 0;
    while (at < data.size()) {
        DWORD n = 0;
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(data.size() - at, 64 * 1024));
        if (!::WriteFile(l_->parent_in.get(), data.data() + at, chunk, &n, nullptr)) return make_error("closed", "the process ended");
        at += n;
    }
    return {};
}

std::string SandboxedProcess::read_some() {
    char buf[16 * 1024];
    DWORD n = 0;
    if (!::ReadFile(l_->parent_out.get(), buf, sizeof buf, &n, nullptr) || n == 0) return {};
    return std::string(buf, n);
}

bool SandboxedProcess::running() const { return ::WaitForSingleObject(l_->process.get(), 0) == WAIT_TIMEOUT; }

int SandboxedProcess::exit_code() const {
    if (running()) return -1;
    DWORD code = 0;
    ::GetExitCodeProcess(l_->process.get(), &code);
    return static_cast<int>(code);
}

std::string SandboxedProcess::stderr_tail() const {
    const std::lock_guard lock(mx_);
    return decode_output(err_tail_);
}

unsigned long SandboxedProcess::pid() const { return ::GetProcessId(l_->process.get()); }

void SandboxedProcess::terminate() { ::TerminateJobObject(l_->job.get(), 1); }

bool SandboxedProcess::wait_ended(std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    if (::WaitForSingleObject(l_->process.get(), static_cast<DWORD>(timeout.count())) != WAIT_OBJECT_0) return false;
    while (!err_done_.load()) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return true;
}

}  // namespace wb::sandbox
