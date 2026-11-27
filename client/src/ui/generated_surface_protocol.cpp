#include "ui/generated_surface.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <initializer_list>
#include <set>
#include <string>
#include <vector>

namespace wb::ui {
namespace {
constexpr size_t kMaxBytes = 65536;
constexpr std::initializer_list<std::string_view> kTemplates = {"daily_report", "meeting_summary", "briefing", "action_prep", "meeting_prep"};

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
bool bounded(const json& j, const char* key, size_t max, bool required = true) {
    if (!j.contains(key)) return !required;
    return j[key].is_string() && !j[key].get_ref<const std::string&>().empty() && j[key].get_ref<const std::string&>().size() <= max;
}
bool one_of(const json& j, const char* key, std::initializer_list<std::string_view> values, bool required = true) {
    if (!j.contains(key)) return !required;
    return j[key].is_string() && std::find(values.begin(), values.end(), j[key].get_ref<const std::string&>()) != values.end();
}
// Rich text of the display components (add-content-templates D2): never a nested surface fence.
bool rich(const json& j, const char* key, size_t max) {
    if (!bounded(j, key, max)) return false;
    const auto& s = j[key].get_ref<const std::string&>();
    return s.find("``skowork-ui") == std::string::npos && s.find("~~skowork-ui") == std::string::npos;
}
// An in-window route ("#/threads/..."), never a web address.
bool route(const json& j) {
    if (!j.contains("link")) return true;
    if (!bounded(j, "link", 512)) return false;
    const auto& s = j["link"].get_ref<const std::string&>();
    return s.starts_with("#/") && std::all_of(s.begin(), s.end(), [](unsigned char c) { return c > 0x20 && c != 0x7f; });
}
bool items(const json& c, size_t max, const std::function<bool(const json&)>& item) {
    if (!arr(c, "items", max) || c["items"].empty()) return false;
    for (const auto& i : c["items"]) if (!i.is_object() || !citations(i) || !item(i)) return false;
    return true;
}
bool strings(const json& j, const char* key, size_t max) {
    if (!j.contains(key)) return true;
    if (!j[key].is_array() || j[key].size() > max) return false;
    for (const auto& v : j[key]) if (!text(v)) return false;
    return true;
}
// The display components of add-content-templates (D2). None of them is a control.
bool display(const json& c, const std::string& type, bool& known) {
    known = true;
    if (type == "markdown") return fields(c, {"type", "id", "label", "content"}) && rich(c, "content", 8192);
    if (type == "callout")
        return fields(c, {"type", "id", "label", "tone", "content"}) && one_of(c, "tone", {"info", "success", "warning", "danger"}) &&
               rich(c, "content", 2048);
    if (type == "facts")
        return fields(c, {"type", "id", "label", "items"}) && items(c, 20, [](const json& i) {
                   return fields(i, {"label", "value"}) && bounded(i, "label", 80) && bounded(i, "value", 512);
               });
    if (type == "timeline")
        return fields(c, {"type", "id", "label", "items"}) && items(c, 50, [](const json& i) {
                   return fields(i, {"time", "title", "detail", "tag"}) && bounded(i, "time", 80) && bounded(i, "title", 4096) &&
                          bounded(i, "detail", 4096, false) && bounded(i, "tag", 80, false);
               });
    if (type == "tasks")
        return fields(c, {"type", "id", "label", "items"}) && items(c, 50, [](const json& i) {
                   return fields(i, {"title", "owner", "due", "state", "link"}) && bounded(i, "title", 4096) &&
                          bounded(i, "owner", 200, false) && bounded(i, "due", 80, false) &&
                          one_of(i, "state", {"open", "done", "overdue"}, false) && route(i);
               });
    if (type == "draft")
        return fields(c, {"type", "id", "label", "to", "cc", "subject", "body"}) && bounded(c, "body", 16384) &&
               strings(c, "to", 20) && strings(c, "cc", 20) && bounded(c, "subject", 1024, false);
    known = false;
    return false;
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
        bool known = false;
        if (const bool ok = display(c, type, known); known) return ok;
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

namespace {
// One example per Content template (add-content-templates D3, D6): each validates and has no controls (unit tested).
struct ContentTemplate {
    std::string_view kind, structure, example;
};
constexpr ContentTemplate kContentTemplates[] = {
    {"daily_report",
     "Components in this order, each only when it has content: timeline \"Today\" with the day's meetings (time, subject, "
     "place or online as tag; a busy-only entry has the title \"Busy\" and nothing else); one markdown per topic of what is "
     "worth knowing, citing every source; tasks \"To act on\" with the Agenda items (due, state overdue when past due); tasks "
     "\"For your information\" with each entry and its reason; tasks \"Action required\" with each entry and link set to its "
     "Thread route.",
     R"({"version":1,"id":"report","template":"daily_report","title":"Friday 27 November","summary":"Two meetings and one overdue item.","components":[{"type":"timeline","id":"today","label":"Today","items":[{"time":"09:30","title":"Steering committee","tag":"Teams"},{"time":"14:00","title":"Busy"}]},{"type":"markdown","id":"budget","label":"Budget","content":"Finance moved the budget deadline to Monday [1].","citations":["1"]},{"type":"tasks","id":"act","label":"To act on","items":[{"title":"Send the Q4 slides to Petra","due":"26 Nov","state":"overdue","link":"#/agenda"}]}]})"},
    {"meeting_summary",
     "Components in this order: facts \"Meeting\" (date, time, participants where known); markdown \"Summary\"; markdown "
     "\"Decisions\" as a list; tasks \"Follow-ups\" with owner and due where the meeting named them.",
     R"({"version":1,"id":"summary","template":"meeting_summary","title":"Order review","summary":"The team agreed to check the order balance.","components":[{"type":"facts","id":"meeting","label":"Meeting","items":[{"label":"Date","value":"27 November 2026"},{"label":"Participants","value":"Klara, Jan"}]},{"type":"markdown","id":"overview","label":"Summary","content":"The team reviewed the open orders."},{"type":"markdown","id":"decisions","label":"Decisions","content":"- Orders over budget need approval."},{"type":"tasks","id":"follow-ups","label":"Follow-ups","items":[{"title":"Check the order balance","owner":"Klara","due":"2 December","state":"open"}]}]})"},
    {"briefing",
     "Components in this order: facts \"Request\" (asked by, when, due, related references); a callout only when the state "
     "matters (already answered: success; missing information or overdue: warning); markdown \"Brief\" citing the sources; "
     "a draft (to, cc, subject, body as plain text) when draft_kind is not none; tasks or markdown with what the user must "
     "still supply.",
     R"({"version":1,"id":"brief","template":"briefing","title":"Reply to Lucie about the invoice","summary":"Lucie needs the invoice confirmed by Friday.","components":[{"type":"facts","id":"request","label":"Request","items":[{"label":"Asked by","value":"Lucie Novak"},{"label":"Due","value":"29 Nov"}]},{"type":"callout","id":"state","label":"Status","tone":"warning","content":"The payment terms are not stated yet."},{"type":"markdown","id":"brief","label":"Brief","content":"Lucie asked you to confirm invoice 2026-118 [1].","citations":["1"]},{"type":"draft","id":"draft","label":"Draft","to":["Lucie Novak"],"subject":"Re: Invoice 2026-118","body":"Hi Lucie,\n\nI confirm the invoice.\n\nJan"},{"type":"tasks","id":"missing","label":"Still to supply","items":[{"title":"The payment terms"}]}]})"},
    {"action_prep",
     "Components in this order: facts \"Request\" (what is asked, by whom, by when); markdown \"Summary\" with what the user "
     "must still supply. The block holds only the summary; the question and the reply follow as instructed.",
     R"({"version":1,"id":"prep","template":"action_prep","title":"Approve the travel request","summary":"Martin asks for approval by Monday.","components":[{"type":"facts","id":"request","label":"Request","items":[{"label":"From","value":"Martin Dvorak"},{"label":"Asked","value":"Approve the trip to Munich"},{"label":"By","value":"30 Nov"}]},{"type":"markdown","id":"summary","label":"Summary","content":"The trip costs 640 EUR [1]. You still need to confirm the cost centre.","citations":["1"]}]})"},
    {"meeting_prep",
     "Components in this order: facts \"Meeting\" (time, place or link, organizer, attendees); markdown \"What it is about\"; "
     "tasks \"Prepare\" with what to prepare or bring; markdown \"Background\" citing the sources.",
     R"({"version":1,"id":"prep","template":"meeting_prep","title":"Steering committee","summary":"The steering committee decides the Atlas budget.","components":[{"type":"facts","id":"meeting","label":"Meeting","items":[{"label":"When","value":"28 Nov, 10:00-11:00"},{"label":"Where","value":"Teams"},{"label":"Organizer","value":"Eva Horak"},{"label":"Attendees","value":"Jan, Petra, Martin"}]},{"type":"markdown","id":"about","label":"What it is about","content":"Approval of the Atlas budget for next year."},{"type":"tasks","id":"prepare","label":"Prepare","items":[{"title":"Bring the updated cost plan"}]},{"type":"markdown","id":"background","label":"Background","content":"Finance asked for a 10 % cut [1].","citations":["1"]}]})"},
};

constexpr std::string_view kContentTemplateFormat =
    "Present the answer as a Content template: first one or two sentences of prose summary, then one top-level fenced block "
    "marked skowork-ui holding strict JSON. Root: {\"version\":1,\"id\":\"...\",\"template\":\"KIND\",\"title\":\"short "
    "title\",\"summary\":\"one sentence\",\"components\":[...]}. Every component has a unique id (ASCII letters, digits, _ and "
    "-), a type and a label, a short heading in the content's language. Types: markdown {content} (Markdown, in-window links "
    "allowed); facts {items:[{label,value}]}; callout {tone: info, success, warning or danger; content}; timeline "
    "{items:[{time,title,detail?,tag?}]}; tasks {items:[{title,owner?,due?,state?: open, done or overdue; link?: an in-window "
    "route starting with #/}]}; draft {to?,cc?: arrays of names or addresses; subject?; body: plain text}; table {columns, "
    "rows}. A component, fact, timeline entry or task may carry citations: an array of this answer's Citation numbers as "
    "strings; never invent one. Use only data you have and leave out a component that would be empty. Use success only for a "
    "fact the sources show, such as a sent reply with its time. No buttons or other controls.\n";
}  // namespace

std::vector<std::string_view> content_template_kinds() {
    std::vector<std::string_view> out;
    for (const auto& t : kContentTemplates) out.push_back(t.kind);
    return out;
}

std::string_view content_template_example(std::string_view kind) {
    for (const auto& t : kContentTemplates)
        if (t.kind == kind) return t.example;
    return {};
}

std::string content_template_instructions(std::string_view kind) {
    for (const auto& t : kContentTemplates) {
        if (t.kind != kind) continue;
        std::string out(kContentTemplateFormat);
        out.replace(out.find("KIND"), 4, kind);
        out.append(t.structure).append("\nFormat example (invented data): ").append(t.example);
        return out;
    }
    return {};
}

bool generated_surface_has_controls(const json& surface) {
    const auto walk = [](const json& components, const auto& self) -> bool {
        if (!components.is_array()) return false;
        for (const auto& c : components) {
            const auto type = jstr(c, "type");
            if (type == "select" || type == "multiselect" || type == "switch" || type == "text" || type == "slider" || type == "button")
                return true;
            if ((type == "section" || type == "card") && c.contains("components") && self(c["components"], self)) return true;
        }
        return false;
    };
    return surface.is_object() && surface.contains("components") && walk(surface["components"], walk);
}

std::string_view generated_surface_example() {
    return R"({"version":1,"id":"comparison","summary":"Atlas requires 12 hours.","components":[{"type":"table","id":"costs","label":"Effort","columns":["Project","Hours"],"rows":[["Atlas",{"text":"12","citations":["1"]}]]},{"type":"bar","id":"chart","label":"Effort","unit":"hours","points":[{"label":"Atlas","value":12,"citations":["1"]}]},{"type":"select","id":"frequency","label":"Reporting frequency","options":["Weekly","Monthly"]},{"type":"button","id":"send","label":"Continue"}]})";
}

std::string_view generated_surface_instructions() {
    return R"surface(When a visual or an explicit user choice would help, provide a concise prose summary followed by a complete top-level fenced block marked skowork-ui containing strict JSON. Do not use this fence for examples. No scripts, URLs, hidden data, tool calls, or additional fields are allowed. Root: {"version":1,"id":"id","summary":"text","components":[...]}. Every component has a unique id, type and label. Types and other fields:
section/card: components array; paragraph: content string; table: columns array of strings and rows of equal-length arrays of strings or {"text":"value","citations":["1"]}; bar/line: unit and points [{"label":"...","value":number}]; diagram: nodes [{"id":"...","label":"..."}] and edges [{"from":"id","to":"id"}]; select/multiselect: options array and optional value (a string or array respectively); switch: boolean value; text: string value; slider: numeric min, max and value; button: label for explicit submission. Display types: markdown: content (Markdown); facts: items [{"label":"...","value":"..."}]; callout: tone (info, success, warning or danger) and content; timeline: items [{"time":"09:30","title":"...","detail":"...","tag":"..."}]; tasks: items [{"title":"...","owner":"...","due":"...","state":"open, done or overdue","link":"#/route"}]; draft: body, optional to and cc arrays and subject (shown with a local Copy action, never sent). Components, facts, timeline entries, tasks, chart points, diagram nodes and edges may carry citations as arrays of existing answer Citation numbers encoded as strings; do not invent sources. IDs contain only ASCII letters, digits, underscores and hyphens, at most 64 bytes. Limits: 64 KiB JSON, 8 component levels, 100 components, 40 controls, 500 table rows, 20 columns, 500 chart points, 100 diagram nodes and 200 edges. Use only supplied data; preserve a readable prose answer.
Choosing components: tables, bar and line charts for data and comparisons; facts, callouts, timelines, tasks and drafts for structured answers; a diagram for a process or structure; controls for choices; no block for a short answer.
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
    if (result.is_discarded() || duplicate || !fields(result, {"version", "id", "summary", "template", "title", "components"}) ||
        result.contains("citations") || !one_of(result, "template", kTemplates, false) || !bounded(result, "title", 200, false) ||
        !result.contains("version") || !result["version"].is_number_integer() || result["version"] != 1 || !result.contains("id") || !id(result["id"]) ||
        !str_field(result, "summary") || !arr(result, "components", 100)) return std::nullopt;
    Validator v;
    for (const auto& c : result["components"]) if (!v.component(c, 1)) return std::nullopt;
    return result;
}

}  // namespace wb::ui
