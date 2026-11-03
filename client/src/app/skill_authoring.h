// Skill authoring in the host (client/skills Skill Authoring, Save Thread As Skill, Skill Test Run, Skill Publishing;
// add-skills design D10): the editor's calls on private Skills and their files, drafts from Threads, test runs in
// labelled test Threads with the draft resolved for that Thread only, and publishing to the skill registry.
#pragma once

#include "host/rpc.h"
#include "runtime/types.h"
#include "skills/authoring.h"

#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace wb {

struct SkillAuthoringEnv {
    std::filesystem::path private_dir;  // <data>/skills/private
    std::function<void()> reload;       // the catalogue reads the disk again
    // Save as skill: the Thread's messages, its title, a tool's declared risk ("" when unknown), the user's name, and
    // the model call attributed to the Thread.
    std::function<std::vector<model::ChatMessage>(const std::string& thread)> conversation;
    std::function<std::string(const std::string& thread)> title;
    std::function<std::string(const std::string& tool)> tool_risk;
    std::function<std::vector<std::string>()> user_names;
    std::function<Result<std::string>(const std::string& thread, const std::string& system, const std::string& user)> model;
    // Test runs: a Thread with the system origin and this reference, and a message sent in it.
    std::function<Result<std::string>(const std::string& title, const json& origin_ref)> create_thread;
    std::function<Status(const std::string& thread, const std::string& text)> send;
    // Publishing: the scopes policy.skills.publish allows, the user's Groups, the registry's publish call (query string
    // and zip) and the user's published versions.
    std::function<std::vector<std::string>()> publish_scopes;
    std::function<std::vector<std::string>()> groups;
    std::function<Result<json>(const std::string& query, const std::string& zip)> publish;
    std::function<Result<json>()> mine;
    // Author Withdrawal: the registry's withdraw call for one version.
    std::function<Result<json>(const std::string& name, const std::string& version)> withdraw;
};

class SkillAuthoring {
public:
    explicit SkillAuthoring(SkillAuthoringEnv env);

    void register_rpc(RpcRegistry& rpc, RpcExposure exposure);

    // The draft a Skill test Thread runs, when `name` is its Skill.
    [[nodiscard]] std::optional<runtime::SkillRun> test_skill(const std::string& thread, const std::string& name) const;
    [[nodiscard]] std::optional<runtime::SkillRun> test_draft(const std::string& thread) const;
    [[nodiscard]] bool is_test_thread(const std::string& thread) const;

    // The calls, for tests and the RPCs.
    Result<json> source(const std::string& name);
    static json validate(const skills::SkillSource& s);
    Result<json> save(const std::string& original, const skills::SkillSource& s);
    Result<json> remove(const std::string& name);
    Result<json> add_files(const std::string& name, const std::string& folder, const std::vector<std::string>& paths);
    Result<json> remove_file(const std::string& name, const std::string& path);
    Result<json> draft(const std::string& thread);
    Result<json> test(const skills::SkillSource& s, const std::string& input);
    Result<json> publish(const std::string& name, const std::string& scope, const std::vector<std::string>& users,
                         const std::vector<std::string>& groups);
    Result<json> published();
    Result<json> withdraw(const std::string& name, const std::string& version);

private:
    std::filesystem::path folder(const std::string& name) const;
    json files_json(const std::string& name) const;

    SkillAuthoringEnv env_;
    mutable std::mutex mx_;
    std::map<std::string, runtime::SkillRun> tests_;  // by test Thread
};

}  // namespace wb
