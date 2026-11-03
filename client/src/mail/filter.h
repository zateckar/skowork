// What may be stored, indexed, sent to a model or returned by a tool (client/mail-calendar Sensitivity Label Exclusion,
// Unlabelled Items, User And Policy Exclusions; design D4): labels from the msip_labels extended property, folder and
// category exclusions from policy and the user, and private calendar items.
#pragma once

#include "base/json.h"
#include "mail/mail_config.h"

#include <string>
#include <vector>

namespace wb::mail {

// The extended property Exchange keeps the MIP label metadata in, and the $expand that asks Graph for it.
inline constexpr const char* kLabelProperty = "String {00020386-0000-0000-C000-000000000046} Name msip_labels";
std::string label_expand();

// The GUID (lower case) of the enabled label in a msip_labels value ("MSIP_Label_<guid>_Enabled=true; ..."), or empty.
std::string parse_msip_labels(std::string_view value);
// The label of a Graph message or event: the enabled label of its msip_labels property, or empty (unlabelled).
std::string item_label(const json& item);

enum class Disposition {
    include,    // handled like any other item
    withheld,   // excluded label or unlabelled under "exclude": only id, label and times are kept
    excluded,   // in an excluded folder or category: not synced, not returned
};

struct Rules {
    std::vector<std::string> excluded_labels;  // lower case GUIDs
    bool exclude_unlabelled = false;
    std::vector<std::string> folders;     // policy plus user; well-known or display names, compared case-insensitively
    std::vector<std::string> categories;  // policy plus user
    bool private_items = true;            // private events are masked to busy time
};

Rules make_rules(const MailConfig& cfg, const std::vector<std::string>& user_folders, const std::vector<std::string>& user_categories);

// `folder_names`: the well-known name and the display name of the item's folder (messages only).
Disposition classify(const Rules& rules, const json& item, const std::vector<std::string>& folder_names = {});
bool folder_excluded(const Rules& rules, const std::vector<std::string>& folder_names);
// An event that is private (sensitivity private or confidential) while private items are excluded.
bool masked_private(const Rules& rules, const json& event);

}  // namespace wb::mail
