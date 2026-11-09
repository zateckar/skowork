// Computer use (client/computer-use; add-computer-use): the computer.* tools that perceive and operate Windows desktop
// applications through UI Automation first and synthesized input second, under the control overlay, the stop hotkey and
// user-input detection. Offered only when policy.tools.computer.mode is allow. Screenshots stay in memory unless policy
// retains them as Thread attachments.
#pragma once

#include "base/result.h"
#include "computer/capture.h"
#include "computer/classify.h"
#include "computer/control.h"
#include "computer/desktop.h"
#include "computer/input.h"
#include "computer/uia.h"
#include "model/image.h"
#include "policy/core.h"
#include "runtime/tools.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace wb::computer {

struct ComputerEnv {
    std::function<std::shared_ptr<const policy::EffectivePolicy>()> policy;
    std::function<model::ImageLimits()> image_limits;  // client/model-access Image Input
    // Whether the gateway maps a model to the vision role; screenshots are offered only then. Unset: available.
    std::function<bool()> vision_available;
    std::function<std::string(const std::string& thread_id)> thread_title;
    // The overlay banner in the UI language, naming the Thread and the stop hotkey.
    std::function<std::wstring(const std::string& thread_title, const std::string& hotkey)> banner;
    // Cancels a Run (the stop hotkey).
    std::function<void(const std::string& run_id, const std::string& reason)> cancel_run;
    std::vector<std::string> own_executables = {"SkoWork.exe", "Workbench.exe", "Workbench-legacy.exe", "workbench-worker.exe"};
    std::chrono::milliseconds uia_limit = std::chrono::seconds(2);  // per UI Automation call (design Risks)
    std::chrono::milliseconds linger = std::chrono::seconds(3);    // the overlay stays this long after an action
    int max_elements = 400;
    // The surface checks (default: the real ones); tests replace them, since a UAC prompt or an elevated window cannot be
    // produced without changing the system.
    std::function<std::optional<std::string>()> unreachable_surface;
    std::function<bool(DWORD pid)> elevated;
};

class ComputerService : public std::enable_shared_from_this<ComputerService> {
public:
    static std::shared_ptr<ComputerService> create(ComputerEnv env);
    ~ComputerService();
    ComputerService(const ComputerService&) = delete;
    ComputerService& operator=(const ComputerService&) = delete;

    // client/computer-use Computer Use Availability and Accessibility Tree Only Mode.
    bool offered();
    bool screenshot_offered();
    // The Risk class of a call, classified by the client (design D5).
    runtime::Risk risk(const std::string& tool, const std::string& run_id, const json& args);
    // Resolves the targets of a call without sending input; a message when the call cannot run (client/computer-use Target
    // Checked Before Approval).
    std::optional<std::string> precheck(const std::string& tool, const std::string& run_id, const json& args);

    runtime::ToolOutput observe(runtime::ToolContext& ctx, const json& args);
    runtime::ToolOutput launch(runtime::ToolContext& ctx, const json& args);
    runtime::ToolOutput screenshot(runtime::ToolContext& ctx, const json& args);
    runtime::ToolOutput click(runtime::ToolContext& ctx, const json& args);
    runtime::ToolOutput type(runtime::ToolContext& ctx, const json& args);
    runtime::ToolOutput keys(runtime::ToolContext& ctx, const json& args);
    runtime::ToolOutput scroll(runtime::ToolContext& ctx, const json& args);
    runtime::ToolOutput drag(runtime::ToolContext& ctx, const json& args);
    runtime::ToolOutput select(runtime::ToolContext& ctx, const json& args);
    // The answer to a Continue/Stop Question this service raised.
    runtime::ToolOutput resume(runtime::ToolContext& ctx, const json& args, const json& state, const std::string& answer);

    // A Run ended: its element references and screenshot transform go, and its control ends.
    void run_ended(const std::string& run_id);
    void shutdown();

    // Tests.
    [[nodiscard]] bool controlling();
    [[nodiscard]] HWND overlay();
    void simulate_user_input();
    [[nodiscard]] InputSender& input() { return input_; }

private:
    struct Ref;
    struct RunState;
    struct Target;
    enum class Halt { none, hotkey, user };

    explicit ComputerService(ComputerEnv env);
    std::string policy_string(const char* key, const std::string& fallback);
    std::vector<std::string> policy_list(const char* key);
    bool tree_only();
    Hotkey hotkey();
    Result<std::shared_ptr<Uia>> uia();
    std::shared_ptr<RunState> run(const std::string& run_id, bool create);

    std::optional<std::string> launch_refusal(const json& args);
    Result<WindowInfo> window_arg(RunState& rs, const json& args);
    Result<Target> resolve(RunState& rs, const json& args, const std::string& prefix, bool need_element);
    std::optional<std::string> scope_refusal(const WindowInfo& w);
    bool listed_own(const WindowInfo& w);
    // Paints every out-of-scope window in a display capture black (client/computer-use Display Capture).
    void black_out(Bitmap& bmp);
    // Checks before any input: the input desktop, application scoping, elevation. A Question or a failure, or nullopt.
    std::optional<runtime::ToolOutput> reachable(runtime::ToolContext& ctx, const std::string& tool, const WindowInfo& w);
    runtime::ToolOutput question(const std::string& kind, const std::string& prompt, json state = json::object());

    // Runs one action under control: the overlay and hooks are up before `body` sends input.
    runtime::ToolOutput act(runtime::ToolContext& ctx, const std::string& tool, const WindowInfo& w,
                            const std::function<runtime::ToolOutput()>& body);
    Status begin_control(const std::string& run_id, const std::string& thread_id, const WindowInfo& w);
    void end_control();
    void on_hotkey();
    void on_user_input();

    runtime::ToolOutput done(const WindowInfo& w, json body);

    ComputerEnv env_;
    std::mutex mx_;  // runs_, uia_
    std::map<std::string, std::shared_ptr<RunState>> runs_;
    std::shared_ptr<Uia> uia_;
    std::mutex act_mx_;  // one action at a time

    std::mutex ctl_mx_;
    std::unique_ptr<ControlSession> control_;
    std::string control_run_;
    unsigned long long control_gen_ = 0;
    std::atomic<bool> armed_{false};  // an action is sending input
    std::atomic<Halt> halt_{Halt::none};
    InputSender input_;
};

// The computer.* tools as runtime tool definitions.
std::vector<runtime::ToolDef> computer_tools(const std::shared_ptr<ComputerService>& service);

}  // namespace wb::computer
