// Chromium IndexedDB key decoding over a LevelDB snapshot of the Teams cache (client/teams; add-local-outlook-and-teams
// design D4): key prefixes, database and object store names from the metadata records, and object-store data records.
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "teams/leveldb.h"

namespace wb::teams {

struct KeyPrefix {
    std::int64_t db_id = 0;
    std::int64_t store_id = 0;
    std::int64_t index_id = 0;
    std::size_t size = 0;  // encoded length in bytes
};

// The KeyPrefix at the start of an IndexedDB LevelDB key, or nullopt when malformed.
std::optional<KeyPrefix> decode_key_prefix(std::string_view key);

// An encoded IndexedDB key (null, string, date, number, array, min key, binary) as display text; advances `in`.
std::optional<std::string> decode_idb_key(std::string_view& in);

struct IdbDatabase {
    std::int64_t id = 0;
    std::string origin;
    std::string name;
};

struct IdbStore {
    std::int64_t db_id = 0;
    std::int64_t id = 0;
    std::string name;
};

struct IdbRecord {
    std::int64_t db_id = 0;
    std::int64_t store_id = 0;
    std::string key_text;
    std::string_view value;  // after the IndexedDB value version varint: the Blink-serialised value
};

class IdbCatalog {
public:
    // Reads the database and object store names. The reader must outlive the catalog.
    static IdbCatalog build(const LevelDbReader& db);

    [[nodiscard]] const std::vector<IdbDatabase>& databases() const { return databases_; }
    [[nodiscard]] const std::vector<IdbStore>& stores() const { return stores_; }
    [[nodiscard]] std::string database_name(std::int64_t db_id) const;

    // Records of every object store named `store_name` (or containing it when store_substring is true) in every
    // database whose name contains `db_name_contains`.
    void for_each_record(std::string_view db_name_contains, std::string_view store_name,
                         const std::function<void(const IdbRecord&)>& fn, bool store_substring = false) const;

private:
    const LevelDbReader* db_ = nullptr;
    std::vector<IdbDatabase> databases_;
    std::vector<IdbStore> stores_;
};

}  // namespace wb::teams
