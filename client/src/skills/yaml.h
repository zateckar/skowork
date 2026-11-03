// A YAML subset for SKILL.md frontmatter (client/skills Skill Package Format): block mappings and sequences by
// indentation, flow lists and maps, quoted and plain scalars, literal (|) and folded (>) block scalars, comments.
// Plain scalars stay strings, except true, false and null. Anchors, tags, multi-documents and complex keys are not
// supported and fail. Portable; the input is untrusted (it comes from shared Skills).
#pragma once

#include "base/json.h"
#include "base/result.h"

#include <string_view>

namespace wb::skills {

struct YamlLimits {
    size_t max_bytes = 64 * 1024;
    int max_depth = 16;
};

// Parses a document whose root is a mapping. Errors name the line (1-based within `text`).
Result<json> parse_yaml(std::string_view text, const YamlLimits& limits = {});

}  // namespace wb::skills
