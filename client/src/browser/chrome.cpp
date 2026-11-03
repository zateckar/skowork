#include "browser/chrome.h"

#include "base/strings.h"
#include "platform/log.h"
#include "platform/registry.h"

#include <cstring>

namespace wb::browser {

namespace {

constexpr char kLog[] = "browser";

std::optional<DWORD> reg_dword(HKEY root, const std::wstring& key, const wchar_t* name) {
    DWORD v = 0, size = sizeof v;
    if (::RegGetValueW(root, key.c_str(), name, RRF_RT_REG_DWORD, nullptr, &v, &size) != ERROR_SUCCESS) return std::nullopt;
    return v;
}

std::string file_version(const std::filesystem::path& exe) {
    DWORD ignored = 0;
    const DWORD size = ::GetFileVersionInfoSizeW(exe.c_str(), &ignored);
    if (size == 0) return {};
    std::vector<unsigned char> buf(size);
    if (!::GetFileVersionInfoW(exe.c_str(), 0, size, buf.data())) return {};
    VS_FIXEDFILEINFO* info = nullptr;
    UINT len = 0;
    if (!::VerQueryValueW(buf.data(), L"\\", reinterpret_cast<void**>(&info), &len) || !info) return {};
    return std::to_string(HIWORD(info->dwFileVersionMS)) + "." + std::to_string(LOWORD(info->dwFileVersionMS)) + "." +
           std::to_string(HIWORD(info->dwFileVersionLS)) + "." + std::to_string(LOWORD(info->dwFileVersionLS));
}

std::wstring quote_arg(const std::wstring& a) {
    if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos) return a;
    std::wstring out = L"\"";
    for (const wchar_t c : a) {
        if (c == L'"') out += L'\\';
        out += c;
    }
    return out + L"\"";
}

}  // namespace

Result<ChromeInfo> find_chrome(const std::filesystem::path& override_exe) {
    std::vector<std::filesystem::path> candidates;
    if (!override_exe.empty()) {
        candidates.push_back(override_exe);
    } else {
        const std::wstring app_paths = L"Software\\Microsoft\\Windows\\CurrentVersion\\App Paths\\chrome.exe";
        for (HKEY root : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE})
            if (auto p = reg_read_string(root, app_paths, L"")) candidates.emplace_back(*p);
        for (const wchar_t* var : {L"ProgramFiles", L"ProgramFiles(x86)", L"LOCALAPPDATA"}) {
            wchar_t buf[MAX_PATH];
            const DWORD n = ::GetEnvironmentVariableW(var, buf, MAX_PATH);
            if (n > 0 && n < MAX_PATH) candidates.push_back(std::filesystem::path(buf) / L"Google" / L"Chrome" / L"Application" / L"chrome.exe");
        }
    }
    for (auto exe : candidates) {
        std::wstring s = exe.wstring();
        if (s.size() >= 2 && s.front() == L'"' && s.back() == L'"') exe = s.substr(1, s.size() - 2);
        std::error_code ec;
        if (!std::filesystem::is_regular_file(exe, ec)) continue;
        ChromeInfo info;
        info.exe = exe;
        info.version = file_version(exe);
        info.major = std::atoi(info.version.c_str());
        if (info.major < kMinChromeMajor)
            return make_error("version", "Chrome " + (info.version.empty() ? std::string("of unknown version") : info.version) +
                                             " is installed; browser use needs Chrome " + std::to_string(kMinChromeMajor) + " or later");
        return info;
    }
    return make_error("not_installed", "Google Chrome is not installed");
}

ChromePolicyState read_chrome_policy(const std::wstring& key) {
    ChromePolicyState st;
    for (HKEY root : {HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER}) {  // machine policy wins
        if (auto v = reg_dword(root, key, L"RemoteDebuggingAllowed"); v && *v == 0) {
            st = {false, "RemoteDebuggingAllowed", "the Chrome policy RemoteDebuggingAllowed is set to false"};
            return st;
        }
        if (auto v = reg_dword(root, key, L"DeveloperToolsAvailability"); v && *v == 2) {
            st = {false, "DeveloperToolsAvailability", "the Chrome policy DeveloperToolsAvailability disallows developer tools"};
            return st;
        }
    }
    return st;
}

std::wstring chrome_command_line(const LaunchOptions& o) {
    std::vector<std::wstring> args = {
        L"--user-data-dir=" + o.profile_dir.wstring(),
        L"--remote-debugging-pipe",
        L"--no-first-run",
        L"--no-default-browser-check",
        L"--disable-background-networking",
        L"--disable-sync",
        L"--disable-default-apps",
        L"--disable-features=Translate,MediaRouter,OptimizationHints",
        L"--metrics-recording-only",
    };
    if (o.headless) args.emplace_back(L"--headless=new");
    if (!o.proxy_server.empty()) args.push_back(L"--proxy-server=" + widen(o.proxy_server));
    for (const auto& a : o.extra_args) args.push_back(a);
    args.emplace_back(L"about:blank");
    std::wstring cmd = quote_arg(o.exe.wstring());
    for (const auto& a : args) cmd += L" " + quote_arg(a);
    return cmd;
}

ChromeProcess::~ChromeProcess() { terminate(); }

bool ChromeProcess::running() const { return process_ && ::WaitForSingleObject(process_.get(), 0) == WAIT_TIMEOUT; }

void ChromeProcess::terminate() {
    if (job_) {
        ::TerminateJobObject(job_.get(), 0);
        job_.reset();
    }
    if (process_) ::WaitForSingleObject(process_.get(), 5000);
    if (cdp_) cdp_.reset();  // the pipe broke with Chrome; joins the reader
}

Result<std::unique_ptr<ChromeProcess>> launch_chrome(const LaunchOptions& o, CdpConnection::EventFn on_event,
                                                    std::function<void()> on_closed) {
    std::error_code ec;
    std::filesystem::create_directories(o.profile_dir, ec);

    // Pipe to Chrome (its fd 3, read) and from Chrome (its fd 4, written).
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE child_read = nullptr, parent_write = nullptr, parent_read = nullptr, child_write = nullptr;
    if (!::CreatePipe(&child_read, &parent_write, &sa, 0)) return make_error("launch", "CreatePipe failed");
    unique_handle cr(child_read), pw(parent_write);
    if (!::CreatePipe(&parent_read, &child_write, &sa, 0)) return make_error("launch", "CreatePipe failed");
    unique_handle pr(parent_read), cw(child_write);
    ::SetHandleInformation(parent_write, HANDLE_FLAG_INHERIT, 0);
    ::SetHandleInformation(parent_read, HANDLE_FLAG_INHERIT, 0);

    // The C runtime's inherited descriptor table (lpReserved2): int count, count flag bytes, count handles. fds 0-2
    // are closed; 3 and 4 are pipes (FOPEN | FPIPE).
    constexpr int kFds = 5;
    std::vector<unsigned char> crt(sizeof(int) + kFds * (1 + sizeof(HANDLE)), 0);
    const int count = kFds;
    std::memcpy(crt.data(), &count, sizeof count);
    unsigned char* flags = crt.data() + sizeof(int);
    unsigned char* handles = flags + kFds;
    const HANDLE fd_handles[kFds] = {INVALID_HANDLE_VALUE, INVALID_HANDLE_VALUE, INVALID_HANDLE_VALUE, child_read, child_write};
    for (int i = 0; i < kFds; ++i) {
        flags[i] = i >= 3 ? static_cast<unsigned char>(0x01 | 0x08) : 0;
        std::memcpy(handles + static_cast<std::size_t>(i) * sizeof(HANDLE), &fd_handles[i], sizeof(HANDLE));
    }

    SIZE_T attr_size = 0;
    ::InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
    std::vector<unsigned char> attr_buf(attr_size);
    auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_buf.data());
    ::InitializeProcThreadAttributeList(attrs, 1, 0, &attr_size);
    HANDLE inherit[2] = {child_read, child_write};
    ::UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, sizeof inherit, nullptr, nullptr);

    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof si;
    si.StartupInfo.cbReserved2 = static_cast<WORD>(crt.size());
    si.StartupInfo.lpReserved2 = crt.data();
    si.lpAttributeList = attrs;

    auto proc = std::unique_ptr<ChromeProcess>(new ChromeProcess());
    proc->job_.reset(::CreateJobObjectW(nullptr, nullptr));
    if (!proc->job_) {
        ::DeleteProcThreadAttributeList(attrs);
        return make_error("launch", "CreateJobObject failed: " + error_text(::GetLastError()));
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
    li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;  // Chrome ends with the client
    ::SetInformationJobObject(proc->job_.get(), JobObjectExtendedLimitInformation, &li, sizeof li);

    std::wstring cmd = chrome_command_line(o);
    PROCESS_INFORMATION pi{};
    const std::wstring dir = o.exe.parent_path().wstring();
    const BOOL ok = ::CreateProcessW(o.exe.c_str(), cmd.data(), nullptr, nullptr, TRUE,
                                     CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT, nullptr,
                                     dir.c_str(), &si.StartupInfo, &pi);
    ::DeleteProcThreadAttributeList(attrs);
    if (!ok) return make_error("launch", "Chrome could not be started: " + error_text(::GetLastError()));
    unique_handle thread(pi.hThread);
    proc->process_.reset(pi.hProcess);
    proc->pid_ = pi.dwProcessId;
    if (!::AssignProcessToJobObject(proc->job_.get(), pi.hProcess)) {
        ::TerminateProcess(pi.hProcess, 1);
        return make_error("launch", "AssignProcessToJobObject failed: " + error_text(::GetLastError()));
    }
    ::ResumeThread(pi.hThread);
    cr.reset();  // Chrome holds its ends now
    cw.reset();

    proc->cdp_ = std::make_unique<CdpConnection>(pw.release(), pr.release(), std::move(on_event), std::move(on_closed));
    auto v = proc->cdp_->call("Browser.getVersion", json::object(), {}, std::chrono::seconds(10));
    if (!v) {
        const bool exited = !proc->running();
        proc->terminate();
        return make_error("handshake", exited ? "Chrome exited before the debugging handshake (is the agent browser profile "
                                                "open in another Chrome, or is remote debugging blocked by policy?)"
                                              : "Chrome did not complete the debugging handshake: " + v.error().message);
    }
    log::info(kLog, "agent browser started: " + jstr(*v, "product") + ", pid " + std::to_string(proc->pid_));
    return proc;
}

}  // namespace wb::browser
