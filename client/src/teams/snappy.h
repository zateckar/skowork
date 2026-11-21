// Raw Snappy block decompression for the Teams cache reader (client/teams; add-local-outlook-and-teams design D4):
// LevelDB table blocks and Chromium's Snappy-wrapped IndexedDB values. Decoding only; strict bounds on untrusted input.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>

#include "base/result.h"

namespace wb::teams {

inline constexpr std::size_t kSnappyMaxOutput = std::size_t{64} << 20;  // 64 MiB

// Decompresses one raw Snappy block (varint length + literal/copy elements). Errors: "snappy" for malformed input,
// "too_large" when the declared length exceeds max_output.
Result<std::string> snappy_decompress(std::string_view in, std::size_t max_output = kSnappyMaxOutput);

}  // namespace wb::teams
