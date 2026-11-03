// browser.snapshot (client/browser-use Page Snapshot With Element References; add-browser-use design D2): the page's
// accessibility tree as compact text, pruned to interactive, landmark and text nodes, with a short reference (e12) for
// every interactive element. References stay the same for the same element while the document stays the same.
#pragma once

#include "base/json.h"

#include <cstddef>
#include <map>
#include <optional>
#include <string>

namespace wb::browser {

// Reference <-> DOM backend node id, for one document of one tab.
class RefTable {
public:
    std::string ref_for(long long backend_id);
    [[nodiscard]] std::optional<long long> resolve(const std::string& ref) const;
    [[nodiscard]] std::size_t size() const { return by_ref_.size(); }
    void reset();  // a new document: every earlier reference is stale

private:
    std::map<long long, std::string> by_node_;
    std::map<std::string, long long> by_ref_;
    long long next_ = 1;
};

struct SnapshotOptions {
    std::size_t max_chars = 24000;  // about 6k tokens
    std::size_t max_text = 160;     // per name, value or text node
};

struct Snapshot {
    std::string text;
    std::size_t interactive = 0;  // elements with a reference
    bool truncated = false;
};

// `nodes` is the `nodes` array of Accessibility.getFullAXTree. Never throws; malformed nodes are skipped.
Snapshot build_snapshot(const json& nodes, RefTable& refs, const SnapshotOptions& options = {});

// Roles that get a reference.
bool is_interactive_role(const std::string& role);

}  // namespace wb::browser
