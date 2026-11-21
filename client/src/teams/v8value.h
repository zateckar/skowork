// Blink/V8 structured-clone deserialisation of IndexedDB values in the Teams cache (client/teams;
// add-local-outlook-and-teams design D4). Covers the Blink envelope, Chromium's Snappy-wrapped values and the V8 tags
// Teams uses; values become JSON. Untrusted input: bounded depth and size, no exceptions, no UB.
#pragma once

#include <string_view>

#include "base/json.h"
#include "base/result.h"

namespace wb::teams {

inline constexpr int kV8MaxDepth = 64;
inline constexpr std::size_t kV8MaxNodes = 1'000'000;

// `value` is an IndexedDB record value after its version varint. Errors (Error::code): "blob" for a value Chromium
// keeps in a blob file, "unsupported" for an unknown or unsupported tag (host objects, errors, wasm), "malformed" for
// truncated or inconsistent bytes, "too_deep" and "too_large" for the limits above. Undefined, holes and skipped
// values (bigints over 64 bits, array buffers) become null; Maps become objects with stringified keys; Sets and
// sparse arrays become arrays; Dates become milliseconds; RegExps become their source text.
Result<json> deserialize_idb_value(std::string_view value);

}  // namespace wb::teams
