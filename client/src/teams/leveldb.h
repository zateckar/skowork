// Read-only LevelDB reader for a snapshot of the Teams IndexedDB cache (client/teams; add-local-outlook-and-teams
// design D4): the live table set comes from the MANIFEST's version edits, tables and write-ahead logs are merged by
// sequence number, and deletions hide older values. Nothing is ever written and the LOCK file is never opened.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <string_view>

#include "base/result.h"

namespace wb::teams {

// CRC-32C (Castagnoli), as LevelDB uses it, and LevelDB's checksum masking.
std::uint32_t crc32c(std::string_view data, std::uint32_t init = 0);
std::uint32_t crc32c_mask(std::uint32_t crc);
std::uint32_t crc32c_unmask(std::uint32_t masked);

// The 32 KiB block log format shared by *.log and MANIFEST-*: calls fn for every reassembled record. Fragments with a
// bad checksum or an impossible length are skipped and counted in *bad_records.
inline constexpr std::size_t kLogBlockSize = 32768;
void read_log(std::string_view file, const std::function<void(std::string_view record)>& fn, int* bad_records = nullptr);

// A WriteBatch record from a log: 8-byte sequence, 4-byte count, then kTypeValue/kTypeDeletion entries.
using BatchFn = std::function<void(std::uint64_t seq, bool deleted, std::string_view key, std::string_view value)>;
Status decode_write_batch(std::string_view record, const BatchFn& fn);

// One table block's entries (prefix-compressed keys, restart array at the end); the trailer is not part of `block`.
Status parse_block(std::string_view block, const std::function<void(std::string_view key, std::string_view value)>& fn);

// All entries of a table file (.ldb/.sst): footer, index block, data blocks (raw or Snappy). A data block with a bad
// checksum or that cannot be decoded is skipped and counted in *bad_blocks. Keys are internal keys.
Status read_table(std::string_view file, const std::function<void(std::string_view internal_key, std::string_view value)>& fn,
                  int* bad_blocks = nullptr);

struct Manifest {
    std::string comparator;
    std::set<std::uint64_t> live_tables;
    std::uint64_t log_number = 0;
    std::uint64_t prev_log_number = 0;
    std::uint64_t next_file = 0;
    std::uint64_t last_sequence = 0;
    int bad_records = 0;
};

// Replays the VersionEdits of a MANIFEST file.
Result<Manifest> parse_manifest(std::string_view file);

// The manifest file name in a CURRENT file's content ("MANIFEST-000123\n"), or empty when it is not one.
std::string manifest_name_from_current(std::string_view current);

class LevelDbReader {
public:
    struct Stats {
        int tables = 0;
        int missing_tables = 0;
        int logs = 0;
        int bad_blocks = 0;
        int bad_log_records = 0;
        int bad_batches = 0;
        std::size_t entries = 0;  // live (not deleted) user keys
        std::size_t deleted = 0;  // user keys whose newest record is a deletion
    };

    // Opens the directory read-only: CURRENT -> MANIFEST -> live tables, then every *.log numbered at or above the
    // manifest's log number. Errors: "not_found" without CURRENT/MANIFEST, "corrupt" for an unusable MANIFEST.
    static Result<LevelDbReader> open(const std::filesystem::path& dir);

    // Live entries in user-key byte order.
    void for_each(const std::function<void(std::string_view user_key, std::string_view value)>& fn) const;
    [[nodiscard]] Stats stats() const { return stats_; }

    // Merges one record (used while loading; exposed for tests): the highest sequence number wins.
    void apply(std::string_view user_key, std::uint64_t seq, bool deleted, std::string_view value);

private:
    struct Entry {
        std::uint64_t seq = 0;
        bool deleted = false;
        std::string value;
    };
    std::map<std::string, Entry, std::less<>> entries_;
    Stats stats_;
};

}  // namespace wb::teams
