#include "scheduler/skill_triggers.h"

#include "base/time.h"

#include <algorithm>
#include <iterator>

namespace wb::scheduler {

namespace {

// Condition fields a SKILL.md may write next to kind instead of under conditions.
constexpr const char* kConditionKeys[] = {"cron", "interval_minutes", "time_zone", "folder", "pattern", "event", "app",
                                          "title_contains", "offset_minutes", "from", "subject_contains", "folder_id"};

json conditions_of(const json& s) {
    json c = s.contains("conditions") && s["conditions"].is_object() ? s["conditions"] : json::object();
    for (const char* k : kConditionKeys)
        if (s.contains(k) && !c.contains(k)) c[k] = s[k];
    return c;
}

}  // namespace

json skill_trigger_def(const std::string& skill, const json& suggestion) {
    json def = json::object();
    if (suggestion.is_object())
        for (const auto& [k, v] : suggestion.items())
            if (std::find(std::begin(kConditionKeys), std::end(kConditionKeys), k) == std::end(kConditionKeys)) def[k] = v;
    def["conditions"] = conditions_of(suggestion);
    def["skill"] = skill;
    if (jstr(def, "name").empty()) def["name"] = skill + " (suggested by the Skill)";
    if (jstr(def, "prompt").empty()) def["prompt"] = "Run the Skill.";
    return def;
}

std::string describe_suggestion(const json& s) {
    const std::string kind = jstr(s, "kind");
    const json c = conditions_of(s);
    std::string what = jstr(s, "name");
    std::string detail;
    if (kind == "schedule") {
        detail = c.contains("interval_minutes") ? "every " + c["interval_minutes"].dump() + " minutes" : "on the schedule " + jstr(c, "cron");
        if (const std::string z = jstr(c, "time_zone"); !z.empty()) detail += " (" + z + ")";
    } else if (kind == "file") {
        detail = "when a file changes in " + jstr(c, "folder");
    } else if (kind == "session") {
        detail = "at " + jstr(c, "event", "logon");
    } else {
        detail = "on " + kind + " events";
    }
    return (what.empty() ? std::string() : what + ": ") + detail;
}

bool SkillTriggerOffers::offer(const std::string& skill, const std::string& version, const json& triggers) {
    if (!triggers.is_array() || triggers.empty() || !env_.db || !env_.ask) return false;
    if (!state(skill, version).empty()) return false;
    std::string prompt = "The Skill \"" + skill + "\" suggests " +
                         (triggers.size() == 1 ? std::string("an automation") : std::to_string(triggers.size()) + " automations") +
                         ". Create " + (triggers.size() == 1 ? "it" : "them") + "?";
    for (const auto& t : triggers) prompt += "\n- " + describe_suggestion(t);
    auto a = env_.ask(prompt, {kCreate, kNotNow});
    if (!a) return false;
    const std::lock_guard lock(env_.db->mutex());
    auto s = env_.db->prepare(
        "INSERT OR REPLACE INTO skill_trigger_offers(skill, version, action_id, state, triggers, created_at) VALUES(?1,?2,?3,'offered',?4,?5)");
    s.bind(1, skill).bind(2, version).bind(3, *a).bind(4, triggers.dump()).bind(5, env_.clock ? env_.clock->unix_millis() : 0);
    return static_cast<bool>(s.run());
}

std::optional<json> SkillTriggerOffers::on_answer(const std::string& action_id, const std::string& answer) {
    if (!env_.db || action_id.empty()) return std::nullopt;
    std::string skill, version, triggers_text;
    {
        const std::lock_guard lock(env_.db->mutex());
        auto s = env_.db->prepare("SELECT skill, version, triggers FROM skill_trigger_offers WHERE action_id=?1 AND state='offered'");
        s.bind(1, action_id);
        if (!s.step()) return std::nullopt;
        skill = s.col_text(0);
        version = s.col_text(1);
        triggers_text = s.col_text(2);
    }
    const bool accept = answer == kCreate;
    json out = {{"created", json::array()}, {"failed", json::array()}};
    if (accept && env_.create)
        for (const auto& t : parse_json(triggers_text).value_or(json::array())) {
            const json def = skill_trigger_def(skill, t);
            if (auto r = env_.create(def)) out["created"].push_back(jstr(*r, "id"));
            else out["failed"].push_back({{"name", jstr(def, "name")}, {"error", r.error().message}});
        }
    const std::lock_guard lock(env_.db->mutex());
    auto s = env_.db->prepare("UPDATE skill_trigger_offers SET state=?1 WHERE skill=?2 AND version=?3");
    s.bind(1, accept ? "accepted" : "declined").bind(2, skill).bind(3, version);
    (void)s.run();
    return out;
}

std::string SkillTriggerOffers::state(const std::string& skill, const std::string& version) {
    if (!env_.db) return {};
    const std::lock_guard lock(env_.db->mutex());
    auto s = env_.db->prepare("SELECT state FROM skill_trigger_offers WHERE skill=?1 AND version=?2");
    s.bind(1, skill).bind(2, version);
    return s.step() ? s.col_text(0) : std::string();
}

}  // namespace wb::scheduler