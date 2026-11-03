#include "browser/snapshot.h"

#include "base/strings.h"

#include <set>
#include <unordered_map>
#include <vector>

namespace wb::browser {

std::string RefTable::ref_for(long long backend_id) {
    if (auto it = by_node_.find(backend_id); it != by_node_.end()) return it->second;
    std::string ref = "e" + std::to_string(next_++);
    by_node_[backend_id] = ref;
    by_ref_[ref] = backend_id;
    return ref;
}

std::optional<long long> RefTable::resolve(const std::string& ref) const {
    if (auto it = by_ref_.find(ref); it != by_ref_.end()) return it->second;
    return std::nullopt;
}

void RefTable::reset() {
    by_node_.clear();
    by_ref_.clear();
    // `next_` keeps counting, so a reference from the previous document never names an element of the new one.
}

bool is_interactive_role(const std::string& role) {
    static const std::set<std::string> roles = {
        "button",   "link",     "textbox",  "searchbox", "combobox", "listbox",          "option",        "checkbox",
        "radio",    "switch",   "slider",   "spinbutton", "menuitem", "menuitemcheckbox", "menuitemradio", "tab",
        "treeitem", "MenuListOption", "MenuListPopup", "PopUpButton", "textField", "ToggleButton",
    };
    return roles.count(role) > 0;
}

namespace {

bool is_structural(const std::string& role) {
    static const std::set<std::string> roles = {
        "heading", "main",  "navigation", "banner", "contentinfo", "form",  "dialog",      "alertdialog", "alert",
        "status",  "article", "search",   "complementary", "table", "grid", "row",         "cell",        "gridcell",
        "columnheader", "rowheader", "list", "listitem", "tablist", "tabpanel", "menu",     "menubar",     "tree",
        "img",     "image", "figure", "region", "group",  "paragraph", "blockquote", "code", "DescriptionList", "term",
        "definition", "caption", "toolbar", "progressbar", "meter", "log", "marquee", "timer", "math", "note",
    };
    return roles.count(role) > 0;
}

// Structural roles that are only worth a line when they have a name.
bool needs_name(const std::string& role) {
    return role == "region" || role == "group" || role == "img" || role == "image" || role == "figure" || role == "paragraph";
}

std::string value_of(const json& field) {
    if (!field.is_object() || !field.contains("value")) return {};
    const json& v = field["value"];
    if (v.is_string()) return v.get<std::string>();
    if (v.is_number_integer()) return std::to_string(v.get<long long>());
    if (v.is_number()) return v.dump();
    if (v.is_boolean()) return v.get<bool>() ? "true" : "false";
    return {};
}

std::string clip(std::string s, std::size_t max) {
    for (char& c : s)
        if (c == '\n' || c == '\r' || c == '\t') c = ' ';
    s = trim(s);
    if (s.size() <= max) return s;
    return utf8_truncate(s, max) + "...";
}

std::string quote(const std::string& s) {
    std::string out = "\"";
    for (const char c : s) {
        if (c == '"' || c == '\\') out.push_back('\\');
        out.push_back(c);
    }
    out.push_back('"');
    return out;
}

struct Node {
    std::string role, name, value;
    bool ignored = false;
    long long backend = 0;
    std::vector<std::string> children;
    std::map<std::string, std::string> props;
};

class Builder {
public:
    Builder(const json& nodes, RefTable& refs, const SnapshotOptions& o) : refs_(refs), o_(o) {
        if (!nodes.is_array()) return;
        for (const auto& n : nodes) {
            if (!n.is_object()) continue;
            const std::string id = jstr(n, "nodeId");
            if (id.empty()) continue;
            Node node;
            node.role = n.contains("role") ? value_of(n["role"]) : std::string{};
            node.name = n.contains("name") ? value_of(n["name"]) : std::string{};
            node.value = n.contains("value") ? value_of(n["value"]) : std::string{};
            node.ignored = jbool(n, "ignored");
            node.backend = jint(n, "backendDOMNodeId");
            if (n.contains("childIds") && n["childIds"].is_array())
                for (const auto& c : n["childIds"])
                    if (c.is_string()) node.children.push_back(c.get<std::string>());
            if (n.contains("properties") && n["properties"].is_array())
                for (const auto& p : n["properties"])
                    if (p.is_object() && p.contains("value")) node.props[jstr(p, "name")] = value_of(p["value"]);
            if (root_.empty() && jstr(n, "parentId").empty()) root_ = id;
            nodes_.emplace(id, std::move(node));
        }
    }

    Snapshot run() {
        if (!root_.empty()) visit(root_, 0, "");
        if (truncated_) out_.text += "[snapshot truncated: take a screenshot or scroll for the rest]\n";
        out_.truncated = truncated_;
        return std::move(out_);
    }

private:
    void line(int depth, const std::string& s) {
        if (truncated_) return;
        const std::string l = std::string(static_cast<std::size_t>(depth) * 2, ' ') + "- " + s + "\n";
        if (out_.text.size() + l.size() > o_.max_chars) {
            truncated_ = true;
            return;
        }
        out_.text += l;
    }

    // All text below a node (for compact table rows).
    void collect_text(const std::string& id, std::string& acc, int guard) {
        auto it = nodes_.find(id);
        if (it == nodes_.end() || guard > 64) return;
        const Node& n = it->second;
        if (n.role == "StaticText" && !n.ignored) {
            if (!acc.empty() && acc.back() != ' ') acc.push_back(' ');
            acc += n.name;
            return;
        }
        for (const auto& c : n.children) collect_text(c, acc, guard + 1);
    }

    bool has_interactive(const std::string& id, int guard) {
        auto it = nodes_.find(id);
        if (it == nodes_.end() || guard > 64) return false;
        if (!it->second.ignored && is_interactive_role(it->second.role)) return true;
        for (const auto& c : it->second.children)
            if (has_interactive(c, guard + 1)) return true;
        return false;
    }

    void visit(const std::string& id, int depth, const std::string& covered) {
        if (truncated_ || !seen_.insert(id).second) return;  // cycles in malformed input
        if (stack_ >= 400) return;                             // absurdly deep trees: stop rather than overflow
        struct Level {
            int& n;
            explicit Level(int& v) : n(v) { ++n; }
            ~Level() { --n; }
        } level(stack_);
        auto it = nodes_.find(id);
        if (it == nodes_.end()) return;
        const Node& n = it->second;
        if (n.role == "InlineTextBox" || n.role == "LineBreak") return;

        if (!n.ignored && n.role == "StaticText") {
            const std::string text = clip(n.name, o_.max_text);
            if (!text.empty() && covered.find(text) == std::string::npos) line(depth, "text: " + quote(text));
            return;
        }

        if (!n.ignored && is_interactive_role(n.role)) {
            std::string s = n.role;
            const std::string name = clip(n.name, o_.max_text);
            if (!name.empty()) s += " " + quote(name);
            s += " [ref=" + refs_.ref_for(n.backend) + "]";
            ++out_.interactive;
            if (!n.value.empty() && n.value != n.name) s += " value=" + quote(clip(n.value, o_.max_text));
            for (const char* p : {"checked", "pressed", "selected", "expanded"}) {
                auto pv = n.props.find(p);
                if (pv != n.props.end() && (pv->second == "true" || pv->second == "mixed")) s += std::string(" [") + p + "]";
            }
            for (const char* p : {"disabled", "required", "readonly"}) {
                auto pv = n.props.find(p);
                if (pv != n.props.end() && pv->second == "true") s += std::string(" [") + p + "]";
            }
            line(depth, s);
            for (const auto& c : n.children) visit(c, depth + 1, n.name);
            return;
        }

        if (!n.ignored && n.role == "row" && !has_interactive(id, 0)) {
            std::vector<std::string> cells;
            for (const auto& c : n.children) {
                std::string t;
                collect_text(c, t, 0);
                cells.push_back(clip(t, o_.max_text));
            }
            std::string joined;
            for (const auto& c : cells) joined += (joined.empty() ? "" : " | ") + c;
            if (!trim(joined).empty()) line(depth, "row: " + quote(joined));
            return;
        }

        if (!n.ignored && is_structural(n.role) && !(needs_name(n.role) && trim(n.name).empty())) {
            std::string s = n.role;
            const std::string name = clip(n.name, o_.max_text);
            if (!name.empty()) s += " " + quote(name);
            if (n.role == "heading") {
                auto lv = n.props.find("level");
                if (lv != n.props.end()) s += " [level=" + lv->second + "]";
            }
            line(depth, s);
            for (const auto& c : n.children) visit(c, depth + 1, n.name);
            return;
        }

        // Generic, ignored and unknown nodes: their children take their place.
        for (const auto& c : n.children) visit(c, depth, covered);
    }

    RefTable& refs_;
    const SnapshotOptions& o_;
    std::unordered_map<std::string, Node> nodes_;
    std::string root_;
    std::set<std::string> seen_;
    Snapshot out_;
    bool truncated_ = false;
    int stack_ = 0;
};

}  // namespace

Snapshot build_snapshot(const json& nodes, RefTable& refs, const SnapshotOptions& options) {
    try {
        return Builder(nodes, refs, options).run();
    } catch (...) {
        return Snapshot{"[the page could not be read]\n", 0, false};
    }
}

}  // namespace wb::browser
