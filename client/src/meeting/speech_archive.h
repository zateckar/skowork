#pragma once
// client/speech-models (improve-skowork-and-knowledge-control D3, task 5.2): the portable reader of a speech package
// archive. It checks the zip's central directory (entry count, names, links and special files, duplicates, declared
// sizes), reads and validates manifest.json, matches every entry to the manifest, and streams an entry while refusing
// bytes beyond its declared size. No Windows API is used, so the fuzz job exercises exactly the production checks.
#include "base/result.h"
#include "meeting/speech_manifest.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace wb::meeting {

struct SpeechLimits {
    std::uint64_t max_archive = 1536ull << 20;  // the zip as downloaded
    std::uint64_t max_total = 1536ull << 20;    // all entries inflated together
    std::uint64_t max_entry = 1024ull << 20;    // one entry inflated
    size_t max_entries = 64;
    // Free bytes on the volume that holds a path; empty uses the system's answer (tests inject disk exhaustion).
    std::function<std::uint64_t(const std::filesystem::path&)> free_space;
};

inline constexpr std::uint64_t kSpeechManifestMaxBytes = 1u << 20;

struct SpeechArchiveEntry {
    std::string name;
    std::uint32_t index = 0;
    std::uint64_t size = 0;  // declared inflated size; extraction refuses more and fails on less
    int listed = -1;         // index into the manifest's files, or -1 for manifest.json and licenses/
};

struct SpeechArchivePlan {
    SpeechManifest manifest;
    std::vector<SpeechArchiveEntry> entries;
    std::uint64_t total = 0;  // all entries inflated
};

// Random access to the archive bytes: returns the number of bytes copied (less only at the end).
using SpeechArchiveRead = std::function<size_t(std::uint64_t offset, void* buf, size_t n)>;

class SpeechArchive {
public:
    SpeechArchive();
    ~SpeechArchive();
    SpeechArchive(const SpeechArchive&) = delete;
    SpeechArchive& operator=(const SpeechArchive&) = delete;

    Status open(SpeechArchiveRead read, std::uint64_t size);
    // Every entry is a regular file named manifest.json, licenses/<name> or a manifest-listed models/stt path (exact
    // case); names are unique without regard to case; counts and declared sizes are within `limits`; every listed file
    // is present with the size the manifest gives.
    Result<SpeechArchivePlan> inspect(const SpeechLimits& limits, const std::function<bool()>& cancelled = {});
    // Streams one entry to `put`; "cancelled" when `cancelled` turns true.
    Status extract(const SpeechArchiveEntry& entry, const std::function<bool(const char*, size_t)>& put,
                   const std::function<bool()>& cancelled = {});

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace wb::meeting
