// A JSON Schema subset validator for tool arguments (add-agent-runtime design D7). Supported keywords:
// type (one name or a list), enum, const, required, properties, additionalProperties (false or a schema), items,
// minItems, maxItems, minLength, maxLength (in code points), minimum, maximum, exclusiveMinimum, exclusiveMaximum,
// anyOf, oneOf. Other keywords are ignored. Portable; never throws.
#pragma once

#include "base/json.h"

#include <string>
#include <vector>

namespace wb {

// Returns the violations (empty when `value` satisfies `schema`), each as "$.path: message". At most max_errors.
std::vector<std::string> validate_json(const json& schema, const json& value, size_t max_errors = 8);

}  // namespace wb
