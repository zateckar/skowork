// Chromium IndexedDB key coding (indexed_db_leveldb_coding) for the Teams cache reader (client/teams;
// add-local-outlook-and-teams design D4).
#include "teams/indexeddb.h"

#include <cmath>
#include <cstdio>
#include <set>
#include <utility>

#include "teams/bytes.h"

namespace wb::teams {
namespace {

constexpr std::uint8_t kDatabaseNameTypeByte = 201;
constexpr std::uint8_t kObjectStoreMetaDataTypeByte = 50;
constexpr std::int64_t kObjectStoreDataIndexId = 1;
constexpr int kMaxKeyDepth = 16;

bool read_le_int(Cursor& c, std::size_t len, std::int64_t& out) {
    std::uint64_t v = 0;
    for (std::size_t i = 0; i < len; ++i) {
        std::uint8_t b = 0;
        if (!c.u8(b)) return false;
        v |= static_cast<std::uint64_t>(b) << (8 * i);
    }
    if (v > static_cast<std::uint64_t>(INT64_MAX)) return false;
    out = static_cast<std::int64_t>(v);
    return true;
}

// StringWithLength: varint count of UTF-16 code units, then big-endian code units.
bool read_string_with_length(Cursor& c, std::string& out) {
    std::uint64_t units = 0;
    if (!c.varint64(units) || units > c.remaining() / 2) return false;
    std::string_view raw;
    if (!c.bytes(static_cast<std::size_t>(units) * 2, raw)) return false;
    out = utf16_to_utf8(raw, /*big_endian=*/true);
    return true;
}

std::string number_text(double d) {
    if (std::isnan(d)) return "NaN";
    if (std::isinf(d)) return d > 0 ? "Infinity" : "-Infinity";
    char buf[40];
    if (d == std::floor(d) && std::fabs(d) < 1e15) std::snprintf(buf, sizeof buf, "%.0f", d);
    else std::snprintf(buf, sizeof buf, "%.17g", d);
    return buf;
}

bool decode_key(Cursor& c, std::string& out, int depth) {
    if (depth > kMaxKeyDepth) return false;
    std::uint8_t type = 0;
    if (!c.u8(type)) return false;
    switch (type) {
        case 0:
            out += "null";
            return true;
        case 1: {
            std::string s;
            if (!read_string_with_length(c, s)) return false;
            out += s;
            return true;
        }
        case 2:
        case 3: {
            double d = 0;
            if (!c.f64(d)) return false;
            out += number_text(d);
            return true;
        }
        case 4: {
            std::uint64_t n = 0;
            if (!c.varint64(n) || n > c.remaining()) return false;
            out += '[';
            for (std::uint64_t i = 0; i < n; ++i) {
                if (i) out += ',';
                if (!decode_key(c, out, depth + 1)) return false;
            }
            out += ']';
            return true;
        }
        case 5:
            out += "-inf";
            return true;
        case 6: {
            std::uint64_t n = 0;
            std::string_view raw;
            if (!c.varint64(n) || n > c.remaining() || !c.bytes(static_cast<std::size_t>(n), raw)) return false;
            static const char* hex = "0123456789abcdef";
            for (const char ch : raw) {
                out += hex[(static_cast<std::uint8_t>(ch) >> 4) & 0xf];
                out += hex[static_cast<std::uint8_t>(ch) & 0xf];
            }
            return true;
        }
        default:
            return false;
    }
}

}  // namespace

std::optional<KeyPrefix> decode_key_prefix(std::string_view key) {
    Cursor c(key);
    std::uint8_t b = 0;
    if (!c.u8(b)) return std::nullopt;
    KeyPrefix p;
    const std::size_t db_len = static_cast<std::size_t>(b >> 5) + 1;
    const std::size_t store_len = static_cast<std::size_t>((b >> 2) & 7) + 1;
    const std::size_t index_len = static_cast<std::size_t>(b & 3) + 1;
    if (!read_le_int(c, db_len, p.db_id) || !read_le_int(c, store_len, p.store_id) || !read_le_int(c, index_len, p.index_id))
        return std::nullopt;
    p.size = c.pos;
    return p;
}

std::optional<std::string> decode_idb_key(std::string_view& in) {
    Cursor c(in);
    std::string out;
    if (!decode_key(c, out, 0)) return std::nullopt;
    in = c.rest();
    return sanitize_utf8(out);
}

IdbCatalog IdbCatalog::build(const LevelDbReader& db) {
    IdbCatalog cat;
    cat.db_ = &db;
    db.for_each([&](std::string_view key, std::string_view value) {
        const auto p = decode_key_prefix(key);
        if (!p || p->store_id != 0 || p->index_id != 0) return;
        Cursor c(key.substr(p->size));
        std::uint8_t type = 0;
        if (!c.u8(type)) return;
        if (p->db_id == 0 && type == kDatabaseNameTypeByte) {
            IdbDatabase d;
            if (!read_string_with_length(c, d.origin) || !read_string_with_length(c, d.name)) return;
            // The value is the database id in Chromium's EncodeInt form: little-endian, minimal length.
            Cursor vc(value);
            if (value.empty() || value.size() > 8 || !read_le_int(vc, value.size(), d.id)) return;
            cat.databases_.push_back(std::move(d));
        } else if (p->db_id > 0 && type == kObjectStoreMetaDataTypeByte) {
            std::uint64_t store_id = 0;
            std::uint8_t meta_type = 0;
            if (!c.varint64(store_id) || !c.u8(meta_type) || meta_type != 0) return;
            if (store_id > static_cast<std::uint64_t>(INT64_MAX)) return;
            cat.stores_.push_back(IdbStore{p->db_id, static_cast<std::int64_t>(store_id), utf16_to_utf8(value, true)});
        }
    });
    return cat;
}

std::string IdbCatalog::database_name(std::int64_t db_id) const {
    for (const auto& d : databases_)
        if (d.id == db_id) return d.name;
    return {};
}

void IdbCatalog::for_each_record(std::string_view db_name_contains, std::string_view store_name,
                                 const std::function<void(const IdbRecord&)>& fn, bool store_substring) const {
    if (!db_) return;
    std::set<std::pair<std::int64_t, std::int64_t>> wanted;
    for (const auto& d : databases_) {
        if (d.name.find(db_name_contains) == std::string::npos) continue;
        for (const auto& s : stores_) {
            if (s.db_id != d.id) continue;
            const bool match = store_substring ? s.name.find(store_name) != std::string::npos : s.name == store_name;
            if (match) wanted.emplace(s.db_id, s.id);
        }
    }
    if (wanted.empty()) return;
    db_->for_each([&](std::string_view key, std::string_view value) {
        const auto p = decode_key_prefix(key);
        if (!p || p->index_id != kObjectStoreDataIndexId || !wanted.count({p->db_id, p->store_id})) return;
        std::string_view rest = key.substr(p->size);
        auto text = decode_idb_key(rest);
        if (!text) return;
        Cursor vc(value);
        std::uint64_t version = 0;
        if (!vc.varint64(version)) return;
        IdbRecord rec;
        rec.db_id = p->db_id;
        rec.store_id = p->store_id;
        rec.key_text = std::move(*text);
        rec.value = vc.rest();
        fn(rec);
    });
}

}  // namespace wb::teams
