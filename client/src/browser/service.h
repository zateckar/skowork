// The agent browser (client/browser-use; add-browser-use design D1-D11): one Chrome on the dedicated profile, shared by
// Runs, each Run in its own tabs. Every request of every page is checked against the egress rules (Fetch
// interception), Chrome's proxy is the egress proxy of client/tool-sandbox, downloads go to the quarantine, and the
// agent never types into credential fields. The browser starts on first use and closes when idle.
#pragma once

#include "base/result.h"
#include "browser/chrome.h"
#include "browser/classify.h"
#include "browser/snapshot.h"
#include "model/types.h"
#include "policy/core.h"
#include "runtime/tools.h"
#include "sandbox/proxy.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace wb::browser {

struct BrowserEnv {
    std::function<std::shared_ptr<const policy::EffectivePolicy>()> policy;
    std::filesystem::path data_root;  // browser-profile, browser-downloads and screenshots live below it
    std::string quarantine_dir;       // client/core-tools quarantine
    sandbox::EgressProxy* proxy = nullptr;  // Chrome's proxy; null: none (tests)
    // Downscales a screenshot for the vision role (client/model-access Image Input).
    std::function<Result<model::ImagePart>(const std::vector<std::uint8_t>& png)> prepare_image;
    std::vector<std::string> idp_hosts;  // identity-provider sign-in hosts (the organization IdP)
    // Records an egress refusal (approval log).
    std::function<void(const std::string& run_id, const std::string& url, const policy::RuleVerdict&)> on_denied;
    // Tests.
    std::filesystem::path chrome_exe;
    std::wstring chrome_policy_key = L"Software\\Policies\\Google\\Chrome";
    bool force_headless = false;
    std::chrono::milliseconds user_input_window = std::chrono::minutes(2);
    std::chrono::milliseconds navigation_timeout = std::chrono::seconds(30);  // then hand over (design D11)
    std::chrono::milliseconds idle_timeout{0};  // overrides policy.tools.browser.idle_timeout_minutes when set
    std::vector<std::wstring> extra_chrome_args;
};

class BrowserService {
public:
    explicit BrowserService(BrowserEnv env);
    ~BrowserService();
    BrowserService(const BrowserService&) = delete;
    BrowserService& operator=(const BrowserService&) = delete;

    struct State {
        std::string state;   // available | blocked | unavailable
        std::string reason;  // why not available
        std::string policy;  // the Chrome policy that blocks debugging
        std::string version;
        bool running = false;
        bool headless = false;
    };
    // The Chrome availability and policy check (rechecked at most once a minute) and whether the browser runs.
    State state();
    // Browser tools are offered while the check passes; browser.eval also needs policy.tools.browser.allow_eval.
    bool offered();
    bool eval_offered();

    // Closes the browser after policy.tools.browser.idle_timeout_minutes without calls, unless the user is using it.
    void tick();
    // A browser tool call started or ended: the idle time counts from here.
    void touch();
    // A Run ended: its tabs close.
    void run_ended(const std::string& run_id);
    void shutdown();

    // The Risk class of a call (design D8).
    runtime::Risk risk(const std::string& tool, const std::string& run_id, const json& args);

    // The tools (client/browser-use Browser Actions).
    runtime::ToolOutput snapshot(runtime::ToolContext& ctx, const json& args);
    runtime::ToolOutput screenshot(runtime::ToolContext& ctx, const json& args);
    runtime::ToolOutput navigate(runtime::ToolContext& ctx, const json& args);
    runtime::ToolOutput click(runtime::ToolContext& ctx, const json& args);
    runtime::ToolOutput type(runtime::ToolContext& ctx, const json& args);
    runtime::ToolOutput select(runtime::ToolContext& ctx, const json& args);
    runtime::ToolOutput scroll(runtime::ToolContext& ctx, const json& args);
    runtime::ToolOutput wait(runtime::ToolContext& ctx, const json& args);
    runtime::ToolOutput tabs(runtime::ToolContext& ctx, const json& args);
    runtime::ToolOutput back(runtime::ToolContext& ctx, const json& args);
    runtime::ToolOutput eval(runtime::ToolContext& ctx, const json& args);
    // The answer to the sign-in hand-over Question (Continue / Stop).
    runtime::ToolOutput resume_handover(runtime::ToolContext& ctx, const json& args, const json& state, const std::string& answer);

    // Tests: the Chrome process id (0 when not running) and the Run's active tab target.
    [[nodiscard]] unsigned long chrome_pid();
    [[nodiscard]] std::string active_target(const std::string& run_id);

private:
    struct Tab;
    struct RunTabs;
    struct Process;
    struct Download {
        std::string guid, url, name, path, error;
        bool done = false;
        long long seq = 0;
    };

    Result<std::shared_ptr<Process>> ensure_browser(bool unattended);
    Result<std::shared_ptr<Tab>> run_tab(runtime::ToolContext& ctx, bool create);
    Result<std::shared_ptr<Tab>> open_tab(const std::shared_ptr<Process>& p, const std::string& run_id, const std::string& url);
    void on_event(const std::weak_ptr<Process>& wp, const CdpMessage& m);
    void on_attached(const std::shared_ptr<Process>& p, const CdpMessage& m);
    void on_request(const std::shared_ptr<Process>& p, const CdpMessage& m);
    void on_download(const CdpMessage& m);
    void close_browser(const std::string& why);
    bool user_interacting(unsigned long pid);

    // Element facts for a reference (design D3, D4); errors "stale_reference".
    Result<ElementFacts> facts(const std::shared_ptr<Process>& p, const std::shared_ptr<Tab>& tab, const std::string& ref);
    Result<std::string> object_for(const std::shared_ptr<Process>& p, const std::shared_ptr<Tab>& tab, const std::string& ref);
    Result<json> call_on(const std::shared_ptr<Process>& p, const std::shared_ptr<Tab>& tab, const std::string& ref,
                         const std::string& function, const json& arguments = json::array());
    // Waits for the page to settle after an action and reports the URL, navigation and downloads.
    json settle(const std::shared_ptr<Process>& p, const std::shared_ptr<Tab>& tab, int navigations_before, long long download_seq,
                std::chrono::milliseconds max_wait);
    std::string page_url(const std::shared_ptr<Tab>& tab);
    runtime::ToolOutput result(const std::shared_ptr<Tab>& tab, json body, bool ok = true);
    runtime::ToolOutput handover(const std::shared_ptr<Process>& p, const std::shared_ptr<Tab>& tab, const std::string& why);
    bool policy_bool(const char* key, bool fallback);
    long long policy_int(const char* key, long long fallback);
    std::string start_site(const std::string& run_id);
    long long download_mark();  // downloads begun after this mark belong to the next action

    BrowserEnv env_;
    std::mutex mx_;  // never held across a CDP call
    std::shared_ptr<Process> proc_;
    std::map<std::string, std::shared_ptr<RunTabs>> runs_;
    std::optional<State> checked_;
    std::chrono::steady_clock::time_point checked_at_{};
    std::chrono::steady_clock::time_point last_call_ = std::chrono::steady_clock::now();
    std::mutex start_mx_;  // serialises starting the browser

    std::mutex dl_mx_;
    std::condition_variable dl_cv_;
    std::map<std::string, Download> downloads_;  // by guid
    long long dl_seq_ = 0;
};

// The browser tools as runtime tool definitions (Declared Risk Classes; the risk of each call from the service).
std::vector<runtime::ToolDef> browser_tools(std::shared_ptr<BrowserService> service);

// The egress decision for one browser request (design D5): non-network schemes (data, blob, about) are allowed, file
// and other local schemes are refused, ws/wss are checked as http/https.
policy::RuleVerdict browser_request_verdict(const policy::EffectivePolicy* policy, const std::string& url);

}  // namespace wb::browser
