#include "ui/generated_surface.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <set>
#include <string>
#include <vector>

namespace wb::ui {
namespace {
constexpr size_t kMaxBytes = 65536;

bool preflight(std::string_view s) {
    if (s.size() > kMaxBytes) return false;
    unsigned depth = 0;
    bool quoted = false, escaped = false;
    for (char c : s) {
        if (quoted) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') quoted = false;
        } else if (c == '"') quoted = true;
        else if (c == '{' || c == '[') { if (++depth > 24) return false; }
        else if (c == '}' || c == ']') { if (!depth) return false; --depth; }
    }
    return !quoted && depth == 0;
}

bool fields(const json& j, std::initializer_list<std::string_view> allowed) {
    if (!j.is_object()) return false;
    for (auto it = j.begin(); it != j.end(); ++it) {
        if (std::find(allowed.begin(), allowed.end(), it.key()) == allowed.end() && it.key() != "citations") return false;
    }
    return true;
}
bool text(const json& j, bool empty = false) {
    return j.is_string() && (empty || !j.get_ref<const std::string&>().empty()) && j.get_ref<const std::string&>().size() <= 4096;
}
bool id(const json& j) {
    if (!j.is_string()) return false;
    const auto& s = j.get_ref<const std::string&>();
    return !s.empty() && s.size() <= 64 && std::all_of(s.begin(), s.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
    });
}
bool citations(const json& j) {
    if (!j.contains("citations")) return true;
    const auto& a = j["citations"];
    return a.is_array() && a.size() <= 20 && std::all_of(a.begin(), a.end(), [](const json& v) {
        if (!v.is_string()) return false;
        const auto& s = v.get_ref<const std::string&>();
        return !s.empty() && s.size() <= 19 && std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; });
    });
}
bool str_field(const json& j, const char* key, bool empty = false) { return j.contains(key) && text(j[key], empty); }
bool num(const json& j) { return j.is_number() && std::isfinite(j.get<double>()); }
bool arr(const json& j, const char* key, size_t max) { return j.contains(key) && j[key].is_array() && j[key].size() <= max; }
bool cell(const json& v) {
    return text(v, true) || (fields(v, {"text", "citations"}) && str_field(v, "text", true) && citations(v));
}
struct Validator {
    std::set<std::string> ids;
    size_t count = 0, controls = 0;
    bool component(const json& c, unsigned depth) {
        if (!c.is_object() || depth > 8 || ++count > 100 || !c.contains("id") || !id(c["id"]) ||
            !ids.insert(c["id"].get<std::string>()).second || !str_field(c, "type") || !str_field(c, "label") || !citations(c)) return false;
        const auto type = c["type"].get<std::string>();
        if (type == "section" || type == "card") {
            if (!fields(c, {"type", "id", "label", "components"}) || !arr(c, "components", 100)) return false;
            for (const auto& child : c["components"]) if (!component(child, depth + 1)) return false;
            return true;
        }
        if (type == "paragraph") return fields(c, {"type", "id", "label", "content"}) && str_field(c, "content");
        if (type == "table") {
            if (!fields(c, {"type", "id", "label", "columns", "rows"}) || !arr(c, "columns", 20) || c["columns"].empty() || !arr(c, "rows", 500)) return false;
            for (const auto& col : c["columns"]) if (!text(col)) return false;
            for (const auto& row : c["rows"]) {
                if (!row.is_array() || row.size() != c["columns"].size()) return false;
                for (const auto& v : row) if (!cell(v)) return false;
            }
            return true;
        }
        if (type == "bar" || type == "line") {
            if (!fields(c, {"type", "id", "label", "unit", "points"}) || !str_field(c, "unit") || !arr(c, "points", 500)) return false;
            for (const auto& p : c["points"]) if (!fields(p, {"label", "value"}) || !citations(p) || !str_field(p, "label") || !p.contains("value") || !num(p["value"])) return false;
            return true;
        }
        if (type == "diagram") {
            if (!fields(c, {"type", "id", "label", "nodes", "edges"}) || !arr(c, "nodes", 100) || !arr(c, "edges", 200)) return false;
            std::set<std::string> nodes;
            for (const auto& n : c["nodes"]) if (!fields(n, {"id", "label"}) || !citations(n) || !n.contains("id") || !id(n["id"]) || !nodes.insert(n["id"].get<std::string>()).second || !str_field(n, "label")) return false;
            for (const auto& e : c["edges"]) if (!fields(e, {"from", "to"}) || !citations(e) || !e.contains("from") || !e.contains("to") || !id(e["from"]) || !id(e["to"]) || !nodes.count(e["from"].get<std::string>()) || !nodes.count(e["to"].get<std::string>())) return false;
            return true;
        }
        if (++controls > 40) return false;
        if (type == "select" || type == "multiselect") {
            if (!fields(c, {"type", "id", "label", "options", "value"}) || !arr(c, "options", 30) || c["options"].empty()) return false;
            std::set<std::string> options;
            for (const auto& option : c["options"]) if (!text(option) || !options.insert(option.get<std::string>()).second) return false;
            if (!c.contains("value")) return true;
            if (type == "select") return text(c["value"]) && options.count(c["value"].get<std::string>());
            if (!c["value"].is_array() || c["value"].size() > options.size()) return false;
            std::set<std::string> selected;
            for (const auto& v : c["value"]) if (!text(v) || !options.count(v.get<std::string>()) || !selected.insert(v.get<std::string>()).second) return false;
            return true;
        }
        if (type == "switch") return fields(c, {"type", "id", "label", "value"}) && c.contains("value") && c["value"].is_boolean();
        if (type == "text") return fields(c, {"type", "id", "label", "value"}) && str_field(c, "value", true);
        if (type == "slider") return fields(c, {"type", "id", "label", "min", "max", "value"}) && c.contains("min") && c.contains("max") && c.contains("value") && num(c["min"]) && num(c["max"]) && num(c["value"]) && c["min"].get<double>() < c["max"].get<double>() && c["min"].get<double>() <= c["value"].get<double>() && c["value"].get<double>() <= c["max"].get<double>();
        return type == "button" && fields(c, {"type", "id", "label"});
    }
};
}  // namespace

std::string_view generated_surface_example() {
    return R"({"version":1,"id":"comparison","summary":"Atlas requires 12 hours.","components":[{"type":"table","id":"costs","label":"Effort","columns":["Project","Hours"],"rows":[["Atlas",{"text":"12","citations":["1"]}]]},{"type":"bar","id":"chart","label":"Effort","unit":"hours","points":[{"label":"Atlas","value":12,"citations":["1"]}]},{"type":"select","id":"frequency","label":"Reporting frequency","options":["Weekly","Monthly"]},{"type":"button","id":"send","label":"Continue"}]})";
}

std::string_view generated_surface_instructions() {
    return R"surface(When a visual or an explicit user choice would help, provide a concise prose summary followed by a complete top-level fenced block marked skowork-ui containing strict JSON. Do not use this fence for examples. No scripts, URLs, hidden data, tool calls, or additional fields are allowed. Root: {"version":1,"id":"id","summary":"text","components":[...]}. Every component has a unique id, type and label. Types and other fields:
section/card: components array; paragraph: content string; table: columns array of strings and rows of equal-length arrays of strings or {"text":"value","citations":["1"]}; bar/line: unit and points [{"label":"...","value":number}]; diagram: nodes [{"id":"...","label":"..."}] and edges [{"from":"id","to":"id"}]; select/multiselect: options array and optional value (a string or array respectively); switch: boolean value; text: string value; slider: numeric min, max and value; button: label for explicit submission. Components, chart points, diagram nodes and edges may carry citations as arrays of existing answer Citation numbers encoded as strings; do not invent sources. IDs contain only ASCII letters, digits, underscores and hyphens, at most 64 bytes. Limits: 64 KiB JSON, 8 component levels, 100 components, 40 controls, 500 table rows, 20 columns, 500 chart points, 100 diagram nodes and 200 edges. Use only supplied data; preserve a readable prose answer.
Choosing components: tables, bar and line charts for data and comparisons; a diagram for a process or structure; controls for choices; no block for a short answer.
Controls return values to you. In an answer block, pressing a button sends you the button label and each control's label and value as the user's next message, so a block with controls and one button lets the user adjust an answer and continue. When you need several choices from the user before you can continue, call ask_user with a short question and a form instead of listing questions in prose: the form is a block of the same format with controls preset to sensible defaults and exactly one button, and the user's values come back as the answer. Do not ask to offer optional extra work: when you have what you need, give the complete answer and mention possible next steps in prose. Form example: {"version":1,"id":"prefs","summary":"Offsite preferences","components":[{"type":"slider","id":"budget","label":"Budget per person (EUR)","min":100,"max":1000,"value":400},{"type":"multiselect","id":"activities","label":"Activities","options":["Hiking","Cooking class","Spa"],"value":["Hiking"]},{"type":"button","id":"plan","label":"Plan it"}]})surface";
}

std::string_view generated_question_form_example() {
    return R"surface({"version":1,"id":"prefs","summary":"Offsite preferences","components":[{"type":"slider","id":"budget","label":"Budget per person (EUR)","min":100,"max":1000,"value":400},{"type":"multiselect","id":"activities","label":"Activities","options":["Hiking","Cooking class","Spa"],"value":["Hiking"]},{"type":"button","id":"plan","label":"Plan it"}]})surface";
}

std::optional<json> parse_generated_surface(std::string_view input) {
    if (!preflight(input)) return std::nullopt;
    bool duplicate = false;
    std::vector<std::set<std::string>> keys;
    auto callback = [&](int, json::parse_event_t event, json& value) {
        if (event == json::parse_event_t::object_start) keys.emplace_back();
        else if (event == json::parse_event_t::key && (!keys.empty() && !keys.back().insert(value.get<std::string>()).second)) duplicate = true;
        else if (event == json::parse_event_t::object_end && !keys.empty()) keys.pop_back();
        return true;
    };
    auto result = json::parse(input.begin(), input.end(), callback, false);
    if (result.is_discarded() || duplicate || !fields(result, {"version", "id", "summary", "components"}) || result.contains("citations") ||
        !result.contains("version") || !result["version"].is_number_integer() || result["version"] != 1 || !result.contains("id") || !id(result["id"]) ||
        !str_field(result, "summary") || !arr(result, "components", 100)) return std::nullopt;
    Validator v;
    for (const auto& c : result["components"]) if (!v.component(c, 1)) return std::nullopt;
    return result;
}

}  // namespace wb::ui
