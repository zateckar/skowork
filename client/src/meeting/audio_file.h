// Encrypted audio tracks on disk (add-meeting-capture design D2): the container of meeting/core.h with a per-track
// AES-256 key wrapped by DPAPI, and the stt worker's transcription request (design D4).
#pragma once

#include "base/json.h"
#include "base/result.h"
#include "meeting/core.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <vector>

namespace wb::meeting {

// Appends 16-bit mono PCM; every second of audio becomes one sealed chunk, written and flushed at once, so a capture
// that ends abruptly loses at most its last second.
class TrackWriter {
public:
    static Result<std::unique_ptr<TrackWriter>> create(const std::filesystem::path& file, TrackKind kind,
                                                       std::uint32_t rate, long long start_ms);
    ~TrackWriter();
    Status append(const std::int16_t* pcm, size_t n);
    // Silence for a gap the device reported (keeps the tracks aligned in time).
    Status append_silence(size_t n);
    Status close();
    [[nodiscard]] std::uint64_t samples() const;

private:
    TrackWriter() = default;
    Status flush_chunk();
    mutable std::mutex mx_;
    std::ofstream out_;
    std::vector<std::uint8_t> key_;
    std::vector<std::int16_t> pending_;
    std::uint32_t rate_ = 16000;
    std::uint64_t written_ = 0;  // samples in sealed chunks
    bool closed_ = false;
};

struct DecodedTrack {
    ContainerHeader header;
    std::vector<std::int16_t> pcm;  // gaps between chunks are filled with silence
    bool truncated = false;
};
// Fails with "corrupt" (container), "crypto" (a key or chunk that does not open) or "io".
Result<DecodedTrack> read_track(const std::filesystem::path& file);

// UTF-8 strings and paths (Windows paths are UTF-16).
std::filesystem::path path_from_utf8(const std::string& s);
std::string path_to_utf8(const std::filesystem::path& p);

// The stt worker's methods: "stt.probe" {speech_dir} -> {ready}, "stt.transcribe" {speech_dir, tracks: [paths], language?, fallback_language?} -> {language, segments,
// engine}; fallback_language is used when the detected language is not supported. Fails with
// "model_missing" when no speech engine is installed (design D4) and "unsupported_language".
Result<json> stt_request(const std::string& method, const json& params);

}  // namespace wb::meeting
