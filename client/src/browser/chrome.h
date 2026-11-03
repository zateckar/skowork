// The agent browser process (client/browser-use Dedicated Agent Browser Profile, Pipe-Only Control Channel and Chrome
// Availability And Policy Check; add-browser-use design D1, D6): finding the managed Chrome, checking its version and
// enterprise policy, and starting it on the dedicated profile with the debugging pipe as its fds 3 and 4.
#pragma once

#include "base/result.h"
#include "browser/cdp.h"
#include "platform/win.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace wb::browser {

inline constexpr int kMinChromeMajor = 136;  // the dedicated --user-data-dir is required for debugging from 136

struct ChromeInfo {
    std::filesystem::path exe;
    std::string version;  // 155.0.8059.80
    int major = 0;
};

// The installed Chrome: App Paths (HKCU, then HKLM), then the standard install folders. `override_exe` replaces the
// search (tests). Errors: "not_installed", "version".
Result<ChromeInfo> find_chrome(const std::filesystem::path& override_exe = {});

struct ChromePolicyState {
    bool allowed = true;
    std::string policy;  // the Chrome policy that blocks debugging (RemoteDebuggingAllowed, DeveloperToolsAvailability)
    std::string reason;
};

// Reads the enterprise Chrome policy (HKLM, then HKCU, under Software\Policies\Google\Chrome). `policy_key` replaces
// the key path (tests).
ChromePolicyState read_chrome_policy(const std::wstring& policy_key = L"Software\\Policies\\Google\\Chrome");

struct LaunchOptions {
    std::filesystem::path exe;
    std::filesystem::path profile_dir;
    bool headless = false;
    std::string proxy_server;               // http://127.0.0.1:<port>, empty: none
    std::vector<std::wstring> extra_args;
};

class ChromeProcess {
public:
    ~ChromeProcess();
    ChromeProcess(const ChromeProcess&) = delete;
    ChromeProcess& operator=(const ChromeProcess&) = delete;

    [[nodiscard]] CdpConnection& cdp() { return *cdp_; }
    [[nodiscard]] unsigned long pid() const { return pid_; }
    [[nodiscard]] bool running() const;
    // Ends Chrome and every process it started (the job object), then closes the connection.
    void terminate();

private:
    friend Result<std::unique_ptr<ChromeProcess>> launch_chrome(const LaunchOptions&, CdpConnection::EventFn,
                                                               std::function<void()>);
    ChromeProcess() = default;
    unique_handle process_, job_;
    unsigned long pid_ = 0;
    std::unique_ptr<CdpConnection> cdp_;
};

// Starts Chrome and completes the CDP handshake (Browser.getVersion) within `handshake_timeout`. Errors: "launch",
// "handshake" (the profile is in use by another Chrome, or debugging is blocked).
Result<std::unique_ptr<ChromeProcess>> launch_chrome(const LaunchOptions& options, CdpConnection::EventFn on_event,
                                                    std::function<void()> on_closed = {});

// The command line launch_chrome uses (exposed for tests).
std::wstring chrome_command_line(const LaunchOptions& options);

}  // namespace wb::browser
