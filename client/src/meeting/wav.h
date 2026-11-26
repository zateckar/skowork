// A 16-bit mono PCM WAV file: meeting audio in evaluation exports, and replayed tracks (add-evaluation-export D2, D4).
#pragma once

#include "base/result.h"

#include <cstdint>
#include <filesystem>
#include <vector>

namespace wb::meeting {

Status write_wav(const std::filesystem::path& file, const std::vector<std::int16_t>& pcm, int sample_rate);
// Reads a file written by write_wav (16-bit mono PCM); fails on anything else.
Result<std::vector<std::int16_t>> read_wav(const std::filesystem::path& file, int* sample_rate = nullptr);

}  // namespace wb::meeting
