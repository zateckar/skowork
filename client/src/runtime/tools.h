// Tool definitions, the registry, Risk class declaration and the `find_tools` search index
// (client/agent-runtime Risk Class Declaration and Lazy Tool Loading; add-agent-runtime design D3).
#pragma once

#include "runtime/hooks.h"
#include "runtime/types.h"

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace wb::runtime {

// ---- Cancellation and child processes ------------------------------------------------------------------------

// What a Run's tools see of cancellation, and how its child processes are terminated (client/agent-runtime
// Cancellation). One per Run.
class RunControl {
public:
    RunControl();
    ~RunControl();
    RunControl(const RunControl&) = delete;
    RunControl& operator=(const RunControl&) = delete;

    // Signals cancellation: closes the in-flight model request, runs the on_cancel callbacks and terminates every
    // process assigned to the Run. Idempotent.
    void cancel(const std::string& reason = "cancelled by user");
    [[nodiscard]] bool cancelled() const { return cancelled_.load(); }
    [[nodiscard]] std::string reason() const;
    [[nodiscard]] const std::shared_ptr<CancelToken>& http_token() const { return http_; }

    // Registers work to do on cancellation (close a socket, stop a browser). Runs at once when already cancelled.
    void on_cancel(std::function<void()> fn);

    // Puts a child process (a Windows process handle) in the Run's job object, so that cancellation terminates it
    // together with everything it spawns. Fails when the process cannot be assigned. A process assigned after
    // cancellation is terminated immediately.
    Status assign_process(void* process_handle);
    // Terminates all processes of the Run now (also done by cancel()).
    void terminate_processes();

private:
    std::atomic<bool> cancelled_{false};
    std::shared_ptr<CancelToken> http_;
    mutable std::mutex mx_;
    std::vector<std::function<void()>> callbacks_;
    std::string reason_;
    void* job_ = nullptr;
};

// ---- Tools ------------------------------------------------------------------------------------------------------

struct ToolContext {
    std::string run_id, thread_id, call_id;
    bool unattended = false;
    // Set when the call comes from a Skill's subagent (client/skills): its name and folder, and whether it declares
    // network none. shell.run applies policy.skills.script_runtimes and the no-network mode from these.
    std::string skill, skill_folder;
    bool skill_no_network = false;
    RunControl* control = nullptr;
    // Adds an entry to the Run's Activity log (for example a retry delay). Unset outside a Run.
    std::function<void(const std::string& text)> activity;
    // The stored result bodies this Run got from one tool, oldest first (client/knowledge-graph Citation Numbers Per
    // Run reads the numbers already given after a restart). Unset outside a Run.
    std::function<std::vector<std::string>(const std::string& tool)> stored_results;
    [[nodiscard]] bool cancelled() const { return control && control->cancelled(); }
};

// A Question raised by a tool or a client feature (client/agent-runtime Client-Raised Questions).
struct QuestionRequest {
    std::string prompt;
    std::vector<std::string> choices;  // fixed choices (empty: free text)
    bool free_text = false;            // the user may type something other than a choice
    json state = nullptr;              // handed back to ToolDef::resume with the answer
};

// A file a tool made for the user, such as a generated image (add-image-generation design D4).
struct ToolFile {
    std::string name;  // image-1.png
    std::string mime;  // image/png
    std::string bytes;
    int width = 0, height = 0;  // pixels, for images
};

struct ToolOutput {
    bool ok = true;
    std::string content;
    // Set when the result came from outside the user's own instructions; client/policy-enforcement defines which
    // sources are untrusted. Defaults to the tool's own provenance.
    std::optional<Provenance> provenance;
    // Interrupts the Run with a Question. The answer goes to ToolDef::resume (never to the model as instructions), or,
    // for a tool without resume, becomes the tool result.
    std::optional<QuestionRequest> question;
    // Images for the model (client/agent-runtime Image Tool Results): sent once, with the vision role, never stored.
    std::vector<model::ImagePart> images;
    // Files for the user: stored as Thread attachments with the tool's provenance and listed in the tool result for the
    // UI. The model is told only their attachment ids, sizes and dimensions; the bytes never go to it.
    std::vector<ToolFile> files;

    static ToolOutput success(std::string content) { return {true, std::move(content), std::nullopt, std::nullopt, {}, {}}; }
    static ToolOutput failure(std::string content) { return {false, std::move(content), std::nullopt, std::nullopt, {}, {}}; }
    static ToolOutput ask(QuestionRequest q) { return {true, {}, std::nullopt, std::move(q), {}, {}}; }
};

using ToolFn = std::function<ToolOutput(ToolContext&, const json& args)>;
using ToolResumeFn = std::function<ToolOutput(ToolContext&, const json& args, const json& state, const std::string& answer)>;

struct ToolDef {
    std::string id;           // files.read, mail.send, mcp:server/tool, ask_user
    std::string description;
    json parameters = {{"type", "object"}, {"properties", json::object()}};  // JSON Schema of the arguments
    // The Risk class. A tool that declares none (and has no risk_of) is evaluated as destructive.
    std::optional<Risk> risk;
    // Computes the Risk class from the arguments of a call (for example shell.run by the command).
    std::function<Risk(const json& args)> risk_of;
    // Computes the Risk class from the Run's state and the arguments (for example a browser click by the page). Takes
    // precedence over risk_of; `risk` then declares the highest class a call can take.
    std::function<Risk(const std::string& run_id, const json& args)> risk_in_run;
    // The lowest Risk class a call of a tool with risk_of or risk_in_run can take (read when unset). Policy withholds such a
    // tool only when it denies every class from here up to the declared one.
    std::optional<Risk> min_risk;
    // Checks a call before the policy decides it: a message ends the call as a failure with no Action item. For a call the
    // tool can tell cannot run (client/computer-use Target Checked Before Approval).
    std::function<std::optional<std::string>(const std::string& run_id, const json& args)> precheck;
    // Whether the tool is offered right now; a withdrawn tool is not listed, found or callable. Unset: always.
    std::function<bool()> offered;
    ToolFn run;
    ToolResumeFn resume;  // continuation after a Question this tool raised
    Provenance provenance;  // source of this tool's results; source_kind empty means "tool result, trusted"
    bool core = false;      // sent on every turn (find_tools, ask_user, tools pinned by the active skill)
    // Touches only the client's own data (Threads, attachments); see ToolCallInfo::own_data.
    bool own_data = false;
    // Offered only in top-level Runs that are not Unattended runs (add-agent-created-skills design D10): a Child Run or
    // an Unattended run does not see the tool, and a call from one fails with an error.
    bool attended_only = false;
    // Extra facts for the Approval payload of a call, computed from the arguments (for example what a Skill save
    // changes). The payload carries them under "preview"; unset: none.
    std::function<json(const json& args)> approval_preview;
    // The files and URLs a call uses, for the policy's argument checks (paths are taken as given; the policy
    // canonicalises them).
    std::function<std::vector<ToolResource>(const json& args)> resources_of;
};

// The Risk class of a call: computed from the Run and arguments or from the arguments, else declared, else destructive.
Risk effective_risk(const ToolDef& tool, const json& args, const std::string& run_id = {});
// Whether the tool is offered right now (ToolDef::offered); a throwing predicate counts as withdrawn.
bool tool_offered(const ToolDef& tool);
// What the policy handler is told about the tool before any call exists (never lower than a call can be).
Risk declared_risk(const ToolDef& tool);
// The lowest Risk class a call can take: min_risk for tools that classify each call, else the declared class.
Risk lowest_risk(const ToolDef& tool);
bool risk_is_declared(const ToolDef& tool);

class ToolRegistry {
public:
    // Replaces a tool with the same id.
    void add(ToolDef tool);
    // Withdraws a tool; returns false when there was none.
    bool remove(const std::string& id);
    [[nodiscard]] const ToolDef* find(const std::string& id) const;
    [[nodiscard]] std::vector<const ToolDef*> all() const;
    [[nodiscard]] size_t size() const { return tools_.size(); }

private:
    std::map<std::string, std::unique_ptr<ToolDef>> tools_;
    // Replaced and withdrawn definitions stay alive: a call already in flight keeps using its ToolDef (tools change
    // only when the MCP catalog changes, so this stays small).
    std::vector<std::unique_ptr<ToolDef>> retired_;
};

// The tool definition as the model gets it. For a tool whose calls can be decided "approve" the schema gets the
// required `rationale` string argument (client/agent-runtime Approval Rationale).
model::ToolSpec wire_spec(const ToolDef& tool, bool with_rationale);

// Rough token count of text and of a tool list (about four characters per token), used for the per-turn schema budget.
size_t estimate_tokens(std::string_view text);
size_t estimate_tool_tokens(const std::vector<model::ToolSpec>& tools);

// BM25 over tool names and descriptions.
class ToolIndex {
public:
    struct Hit {
        std::string id;
        double score = 0;
    };
    explicit ToolIndex(const std::vector<const ToolDef*>& tools);
    // The best matches for `query`, best first. Tools without any matching term are not returned.
    [[nodiscard]] std::vector<Hit> search(const std::string& query, size_t limit) const;

private:
    struct Doc {
        std::string id;
        std::map<std::string, int> terms;
        int length = 0;
    };
    std::vector<Doc> docs_;
    std::map<std::string, int> doc_freq_;
    double avg_length_ = 1;
};

std::vector<std::string> tokenize(std::string_view text);

}  // namespace wb::runtime
