#include "eval/registry.h"

#include "agenda/core.h"
#include "base/encoding.h"
#include "knowledge/core.h"
#include "platform/crypto.h"
#include "triage/classifier.h"

#include <functional>
#include <map>
#include <vector>

namespace wb::eval {
namespace {

enum class Kind { text, number, choice };

struct Entry {
    std::string name;
    Step step = Step::triage;
    Kind kind = Kind::text;
    std::function<json()> fallback;  // the live default
    double min = 0, max = 0;
    std::vector<std::string> choices;
};

// Numbers mirror the live constants: triage kBatch and kMaxEventText (triage/triage.h), the speech engine's
// diarization threshold, hotword score, beam paths and VAD threshold (meeting/sherpa_engine.cpp).
Entry text_entry(std::string name, Step step, std::function<json()> fallback) {
    Entry e;
    e.name = std::move(name);
    e.step = step;
    e.kind = Kind::text;
    e.fallback = std::move(fallback);
    return e;
}

Entry number_entry(std::string name, Step step, double fallback, double min, double max) {
    Entry e;
    e.name = std::move(name);
    e.step = step;
    e.kind = Kind::number;
    e.fallback = [fallback] { return json(fallback); };
    e.min = min;
    e.max = max;
    return e;
}

Entry choice_entry(std::string name, Step step, std::string fallback, std::vector<std::string> choices) {
    Entry e;
    e.name = std::move(name);
    e.step = step;
    e.kind = Kind::choice;
    e.fallback = [fallback] { return json(fallback); };
    e.choices = std::move(choices);
    return e;
}

const std::vector<Entry>& entries() {
    static const std::vector<Entry> all = {
        text_entry("triage.instructions", Step::triage, [] { return json(triage::classification_instructions()); }),
        number_entry("triage.batch_size", Step::triage, 20, 1, 20),
        number_entry("triage.body_limit", Step::triage, 8192, 1024, 8192),
        text_entry("agenda.instructions", Step::agenda, [] { return json(agenda::detection_instructions()); }),
        text_entry("agenda.merge_instructions", Step::agenda, [] { return json(agenda::merge_instructions()); }),
        text_entry("knowledge.extraction", Step::knowledge, [] { return json(knowledge::extraction_system_text()); }),
        text_entry("knowledge.score", Step::knowledge, [] { return json(knowledge::score_instructions()); }),
        text_entry("knowledge.resolve", Step::knowledge, [] { return json(knowledge::resolve_instructions()); }),
        number_entry("diarize.threshold", Step::diarize, 0.5, 0.3, 0.9),
        choice_entry("diarize.clusters", Step::diarize, "auto", {"auto", "attendees"}),
        number_entry("asr.hotword_score", Step::asr, 2.0, 0.0, 5.0),
        number_entry("asr.beam_paths", Step::asr, 4, 1, 8),
        number_entry("asr.vad_threshold", Step::asr, 0.5, 0.2, 0.8),
    };
    return all;
}

const Entry* find_entry(std::string_view name) {
    for (const auto& e : entries())
        if (e.name == name) return &e;
    return nullptr;
}

Error rejected(const std::string& key, const std::string& why) {
    return make_error("invalid_variant", "Variant key " + key + ": " + why);
}

}  // namespace

std::string_view to_string(Step step) {
    switch (step) {
        case Step::triage: return "triage";
        case Step::agenda: return "agenda";
        case Step::knowledge: return "knowledge";
        case Step::diarize: return "diarize";
        case Step::asr: return "asr";
    }
    return "triage";
}

std::optional<Step> parse_step(std::string_view name) {
    for (Step s : {Step::triage, Step::agenda, Step::knowledge, Step::diarize, Step::asr})
        if (to_string(s) == name) return s;
    return std::nullopt;
}

Result<Variant> Variant::parse(Step step, const json& overrides) {
    if (!overrides.is_object()) return make_error("invalid_variant", "A variant is a JSON object of named overrides.");
    for (const auto& [key, value] : overrides.items()) {
        const Entry* e = find_entry(key);
        if (!e) return rejected(key, "unknown name");
        if (e->step != step) return rejected(key, "belongs to the step " + std::string(to_string(e->step)));
        switch (e->kind) {
            case Kind::text:
                if (!value.is_string() || value.get<std::string>().empty()) return rejected(key, "must be non-empty text");
                break;
            case Kind::number: {
                if (!value.is_number()) return rejected(key, "must be a number");
                const double v = value.get<double>();
                if (v < e->min || v > e->max) return rejected(key, "must be between the allowed limits");
                break;
            }
            case Kind::choice: {
                if (!value.is_string()) return rejected(key, "must be one of the allowed choices");
                bool ok = false;
                for (const auto& c : e->choices) ok = ok || c == value.get<std::string>();
                if (!ok) return rejected(key, "must be one of the allowed choices");
                break;
            }
        }
    }
    return Variant(step, overrides);
}

const Variant& Variant::defaults(Step step) {
    static const std::map<Step, Variant> all = [] {
        std::map<Step, Variant> m;
        for (Step s : {Step::triage, Step::agenda, Step::knowledge, Step::diarize, Step::asr})
            m.emplace(s, Variant(s, json::object()));
        return m;
    }();
    return all.at(step);
}

json Variant::effective() const {
    json out = json::object();
    for (const auto& e : entries())
        if (e.step == step_) out[e.name] = overrides_.contains(e.name) ? overrides_[e.name] : e.fallback();
    return out;
}

std::string Variant::text(std::string_view name) const {
    const json all = effective();
    const auto it = all.find(std::string(name));
    return it != all.end() && it->is_string() ? it->get<std::string>() : std::string{};
}

double Variant::number(std::string_view name) const {
    const json all = effective();
    const auto it = all.find(std::string(name));
    return it != all.end() && it->is_number() ? it->get<double>() : 0.0;
}

std::string Variant::choice(std::string_view name) const { return text(name); }

std::string Variant::instruction_id() const {
    // nlohmann::json objects keep their keys sorted, so dump() is canonical.
    return hex_encode(sha256(effective().dump())).substr(0, 12);
}

}  // namespace wb::eval
