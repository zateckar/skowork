// client/skills Suggested Triggers (add-skills task 2.4): a Skill's triggers field is offered once per Skill version as a
// standalone Question; only the user's "Create" makes Triggers through client/scheduler, which validates each one.
#pragma once

#include "base/json.h"
#include "base/result.h"
#include "base/time.h"
#include "storage/database.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace wb::scheduler {

// The Trigger definition for one suggestion: the suggestion's fields, the Skill's name as `skill`, and a name and prompt
// when the suggestion has none.
json skill_trigger_def(const std::string& skill, const json& suggestion);
// One line per suggestion for the Question ("Every Monday at 08:00 (0 8 * * 1)", "When a file changes in C:\Reports").
std::string describe_suggestion(const json& suggestion);

struct SkillTriggerEnv {
    Database* db = nullptr;
    const Clock* clock = nullptr;
    // Raises the standalone Question (capability "skill-triggers"); returns its Action item id.
    std::function<Result<std::string>(const std::string& prompt, const std::vector<std::string>& choices)> ask;
    // Creates a Trigger (Scheduler::create). The user confirmed it, so it is the user's; the definition names the Skill.
    std::function<Result<json>(const json& def)> create;
};

class SkillTriggerOffers {
public:
    explicit SkillTriggerOffers(SkillTriggerEnv env) : env_(std::move(env)) {}
    // Offers the Skill version's suggested Triggers unless that version was offered before. Returns whether it asked.
    bool offer(const std::string& skill, const std::string& version, const json& triggers);
    // The answer to the Question: "Create" makes the Triggers; anything else declines. Returns what was created
    // ({created: [ids], failed: [{name, error}]}) or nothing when the Action item is not an offer.
    std::optional<json> on_answer(const std::string& action_id, const std::string& answer);
    // offered | accepted | declined, or empty when this version was never offered.
    [[nodiscard]] std::string state(const std::string& skill, const std::string& version);

    static constexpr const char* kCreate = "Create";
    static constexpr const char* kNotNow = "Not now";

private:
    SkillTriggerEnv env_;
};

}  // namespace wb::scheduler