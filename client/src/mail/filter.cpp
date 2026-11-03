#include "mail/filter.h"

#include "base/strings.h"

#include <algorithm>

namespace wb::mail {

std::string label_expand() { return std::string("singleValueExtendedProperties($filter=id eq '") + kLabelProperty + "')"; }

std::string parse_msip_labels(std::string_view value) {
    // Entries are separated by ';'; the enabled label is the one with MSIP_Label_<guid>_Enabled=true.
    size_t pos = 0;
    while (pos < value.size()) {
        size_t end = value.find(';', pos);
        if (end == std::string_view::npos) end = value.size();
        std::string entry(value.substr(pos, end - pos));
        pos = end + 1;
        const auto first = entry.find_first_not_of(" \t");
        if (first == std::string::npos) continue;
        entry = entry.substr(first);
        // Compared case-insensitively: the GUID's case varies between clients.
        constexpr std::string_view prefix = "msip_label_";
        constexpr std::string_view suffix = "_enabled=true";
        while (!entry.empty() && (entry.back() == ' ' || entry.back() == '\t')) entry.pop_back();
        const std::string lower = to_lower(entry);
        if (lower.size() <= prefix.size() + suffix.size() || lower.compare(0, prefix.size(), prefix) != 0) continue;
        if (lower.compare(lower.size() - suffix.size(), suffix.size(), suffix) != 0) continue;
        const std::string guid = lower.substr(prefix.size(), lower.size() - prefix.size() - suffix.size());
        if (guid.size() == 36) return guid;
    }
    return {};
}

std::string item_label(const json& item) {
    const auto it = item.find("singleValueExtendedProperties");
    if (it == item.end() || !it->is_array()) return {};
    for (const auto& p : *it) {
        if (!p.is_object() || to_lower(jstr(p, "id")) != to_lower(kLabelProperty)) continue;
        return parse_msip_labels(jstr(p, "value"));
    }
    return {};
}

Rules make_rules(const MailConfig& cfg, const std::vector<std::string>& user_folders, const std::vector<std::string>& user_categories) {
    Rules r;
    r.excluded_labels = cfg.excluded_labels;
    r.exclude_unlabelled = cfg.exclude_unlabelled;
    for (const auto& f : cfg.exclusions.folders) r.folders.push_back(to_lower(f));
    for (const auto& f : user_folders) r.folders.push_back(to_lower(f));
    for (const auto& c : cfg.exclusions.categories) r.categories.push_back(to_lower(c));
    for (const auto& c : user_categories) r.categories.push_back(to_lower(c));
    r.private_items = cfg.exclusions.private_items;
    return r;
}

bool folder_excluded(const Rules& rules, const std::vector<std::string>& folder_names) {
    for (const auto& n : folder_names)
        if (!n.empty() && std::find(rules.folders.begin(), rules.folders.end(), to_lower(n)) != rules.folders.end()) return true;
    return false;
}

Disposition classify(const Rules& rules, const json& item, const std::vector<std::string>& folder_names) {
    if (folder_excluded(rules, folder_names)) return Disposition::excluded;
    if (const auto c = item.find("categories"); c != item.end() && c->is_array())
        for (const auto& x : *c)
            if (x.is_string() && std::find(rules.categories.begin(), rules.categories.end(), to_lower(x.get<std::string>())) != rules.categories.end())
                return Disposition::excluded;
    const std::string label = item_label(item);
    if (label.empty()) return rules.exclude_unlabelled ? Disposition::withheld : Disposition::include;
    if (std::find(rules.excluded_labels.begin(), rules.excluded_labels.end(), label) != rules.excluded_labels.end()) return Disposition::withheld;
    return Disposition::include;
}

bool masked_private(const Rules& rules, const json& event) {
    if (!rules.private_items) return false;
    const std::string s = to_lower(jstr(event, "sensitivity"));
    return s == "private" || s == "confidential";
}

}  // namespace wb::mail
