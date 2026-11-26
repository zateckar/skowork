// The named model instructions and tunable settings of the steps that evaluation can replay (client/evaluation Replay
// Variants; add-evaluation-export design D3). Live features keep their own defaults; the registry mirrors them (a test
// keeps the two equal) and a Variant overlays a replay's overrides on them. Live features never read a Variant.
#pragma once

#include "base/json.h"
#include "base/result.h"

#include <optional>
#include <string>
#include <string_view>

namespace wb::eval {

enum class Step { triage, agenda, knowledge, diarize, asr };

std::string_view to_string(Step step);
std::optional<Step> parse_step(std::string_view name);

class Variant {
public:
    // Overrides for one step: an object whose keys are entry names of that step. Unknown names, names of another
    // step, wrong types and values outside an entry's range are rejected, naming the key.
    static Result<Variant> parse(Step step, const json& overrides);
    // The live defaults of a step (no overrides).
    static const Variant& defaults(Step step);

    [[nodiscard]] std::string text(std::string_view name) const;
    [[nodiscard]] double number(std::string_view name) const;
    [[nodiscard]] std::string choice(std::string_view name) const;
    // The first 12 hex characters of the SHA-256 of the canonical JSON of every effective entry of the step.
    [[nodiscard]] std::string instruction_id() const;
    [[nodiscard]] Step step() const { return step_; }
    [[nodiscard]] const json& overrides() const { return overrides_; }
    // Every effective entry, by name.
    [[nodiscard]] json effective() const;

private:
    Variant(Step step, json overrides) : step_(step), overrides_(std::move(overrides)) {}
    Step step_;
    json overrides_;
};

}  // namespace wb::eval
