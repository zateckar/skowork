// Teams cache decoder tests (client/teams; add-local-outlook-and-teams design D4, D5). The test-only encoders below
// produce Snappy blocks, LevelDB logs, tables and manifests, IndexedDB keys and V8 values, so every decoder is checked
// against bytes with known content, and against every truncation of them.
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "teams/indexeddb.h"
#include "teams/leveldb.h"
#include "teams/snappy.h"
#include "teams/snapshot.h"
#include "teams/teams_extract.h"
#include "teams/v8value.h"

using namespace wb;
using namespace wb::teams;
namespace fs = std::filesystem;

namespace {

// ---- Generic encoders -------------------------------------------------------------------------------------------

std::string varint(std::uint64_t v) {
    std::string out;
    while (v >= 0x80) {
        out += static_cast<char>((v & 0x7f) | 0x80);
        v >>= 7;
    }
    out += static_cast<char>(v);
    return out;
}

std::string fixed32(std::uint32_t v) {
    std::string out;
    for (int i = 0; i < 4; ++i) out += static_cast<char>((v >> (8 * i)) & 0xff);
    return out;
}

std::string fixed64(std::uint64_t v) {
    std::string out;
    for (int i = 0; i < 8; ++i) out += static_cast<char>((v >> (8 * i)) & 0xff);
    return out;
}

std::string lslice(std::string_view s) { return varint(s.size()) + std::string(s); }

// ---- Snappy (greedy compressor with copy-2 elements) ---------------------------------------------------------------

std::string snappy_compress(std::string_view in) {
    std::string out = varint(in.size());
    auto emit_literal = [&](std::size_t from, std::size_t to) {
        while (from < to) {
            const std::size_t n = std::min<std::size_t>(to - from, 60);
            out += static_cast<char>((n - 1) << 2);
            out.append(in.substr(from, n));
            from += n;
        }
    };
    std::unordered_map<std::string, std::size_t> last;
    std::size_t i = 0, lit = 0;
    while (i + 4 <= in.size()) {
        const std::string key(in.substr(i, 4));
        const auto it = last.find(key);
        if (it != last.end() && i - it->second <= 65535) {
            const std::size_t off = i - it->second;
            std::size_t len = 4;
            while (i + len < in.size() && len < 64 && in[i + len] == in[i + len - off]) ++len;
            emit_literal(lit, i);
            out += static_cast<char>(((len - 1) << 2) | 2);
            out += static_cast<char>(off & 0xff);
            out += static_cast<char>(off >> 8);
            last[key] = i;
            i += len;
            lit = i;
        } else {
            last[key] = i;
            ++i;
        }
    }
    emit_literal(lit, in.size());
    return out;
}

// ---- LevelDB ------------------------------------------------------------------------------------------------------

struct LogWriter {
    std::string out;
    void add(std::string_view rec) {
        bool begin = true;
        for (;;) {
            std::size_t left = kLogBlockSize - out.size() % kLogBlockSize;
            if (left < 7) {
                out.append(left, '\0');
                left = kLogBlockSize;
            }
            const std::size_t frag = std::min(left - 7, rec.size());
            const bool end = frag == rec.size();
            const char type = begin && end ? 1 : begin ? 2 : end ? 4 : 3;
            const std::string data(rec.substr(0, frag));
            const std::uint32_t crc = crc32c_mask(crc32c(data, crc32c(std::string(1, type))));
            out += fixed32(crc);
            out += static_cast<char>(frag & 0xff);
            out += static_cast<char>(frag >> 8);
            out += type;
            out += data;
            rec.remove_prefix(frag);
            begin = false;
            if (end) break;
        }
    }
};

struct BatchOp {
    bool del;
    std::string key, value;
};

std::string write_batch(std::uint64_t seq, const std::vector<BatchOp>& ops) {
    std::string out = fixed64(seq) + fixed32(static_cast<std::uint32_t>(ops.size()));
    for (const auto& op : ops) {
        out += static_cast<char>(op.del ? 0 : 1);
        out += lslice(op.key);
        if (!op.del) out += lslice(op.value);
    }
    return out;
}

std::string ikey(std::string_view user, std::uint64_t seq, bool del = false) {
    return std::string(user) + fixed64((seq << 8) | (del ? 0u : 1u));
}

std::string build_block(const std::vector<std::pair<std::string, std::string>>& entries, std::size_t interval = 2) {
    std::string out, prev;
    std::vector<std::uint32_t> restarts;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const auto& [k, v] = entries[i];
        std::size_t shared = 0;
        if (i % interval == 0) {
            restarts.push_back(static_cast<std::uint32_t>(out.size()));
        } else {
            while (shared < prev.size() && shared < k.size() && prev[shared] == k[shared]) ++shared;
        }
        out += varint(shared) + varint(k.size() - shared) + varint(v.size()) + k.substr(shared) + v;
        prev = k;
    }
    if (restarts.empty()) restarts.push_back(0);
    for (const auto r : restarts) out += fixed32(r);
    out += fixed32(static_cast<std::uint32_t>(restarts.size()));
    return out;
}

// A table with the entries split into data blocks of `per_block` entries each.
std::string build_table(const std::vector<std::pair<std::string, std::string>>& entries, bool snappy, std::size_t per_block = 2) {
    std::string file;
    auto add_block = [&](const std::string& contents, bool compress) {
        const std::string data = compress ? snappy_compress(contents) : contents;
        const char type = compress ? 1 : 0;
        const std::string handle = varint(file.size()) + varint(data.size());
        file += data;
        file += type;
        file += fixed32(crc32c_mask(crc32c(data + type)));
        return handle;
    };
    std::vector<std::pair<std::string, std::string>> index;
    for (std::size_t i = 0; i < entries.size(); i += per_block) {
        std::vector<std::pair<std::string, std::string>> chunk(entries.begin() + static_cast<std::ptrdiff_t>(i),
                                                              entries.begin() + static_cast<std::ptrdiff_t>(std::min(entries.size(), i + per_block)));
        index.emplace_back(chunk.back().first, add_block(build_block(chunk), snappy));
    }
    const std::string meta = add_block(build_block({}), false);
    const std::string idx = add_block(build_block(index, 1), false);
    std::string footer = meta + idx;
    footer.resize(40, '\0');
    footer += fixed64(0xdb4775248b80fb57ull);
    return file + footer;
}

struct Edit {
    std::string comparator;
    std::uint64_t log_number = 0, next_file = 0, last_seq = 0;
    std::vector<std::uint64_t> added, deleted;
};

std::string version_edit(const Edit& e) {
    std::string out;
    if (!e.comparator.empty()) out += varint(1) + lslice(e.comparator);
    if (e.log_number) out += varint(2) + varint(e.log_number);
    if (e.next_file) out += varint(3) + varint(e.next_file);
    if (e.last_seq) out += varint(4) + varint(e.last_seq);
    for (const auto n : e.deleted) out += varint(6) + varint(0) + varint(n);
    for (const auto n : e.added) out += varint(7) + varint(0) + varint(n) + varint(100) + lslice(ikey("a", 1)) + lslice(ikey("z", 1));
    return out;
}

void write_file(const fs::path& p, std::string_view data) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(data.data(), static_cast<std::streamsize>(data.size()));
}

std::string read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), {});
}

fs::path fresh_dir(const std::string& name) {
    static std::atomic<int> counter{0};
    const fs::path p = fs::temp_directory_path() / ("wb_teams_test_" + name + "_" + std::to_string(counter++));
    std::error_code ec;
    fs::remove_all(p, ec);
    fs::create_directories(p);
    return p;
}

// Writes a database: one table per element of `tables` (number -> entries), a manifest listing `live` with the given
// log number, and logs (number -> batches).
void write_db(const fs::path& dir, const std::map<std::uint64_t, std::vector<std::pair<std::string, std::string>>>& tables,
              const std::vector<Edit>& edits, const std::map<std::uint64_t, std::vector<std::string>>& logs, bool snappy = false) {
    for (const auto& [n, entries] : tables) {
        char name[32];
        std::snprintf(name, sizeof name, "%06llu.ldb", static_cast<unsigned long long>(n));
        write_file(dir / name, build_table(entries, snappy));
    }
    LogWriter manifest;
    for (const auto& e : edits) manifest.add(version_edit(e));
    write_file(dir / "MANIFEST-000002", manifest.out);
    write_file(dir / "CURRENT", "MANIFEST-000002\n");
    for (const auto& [n, batches] : logs) {
        LogWriter w;
        for (const auto& b : batches) w.add(b);
        char name[32];
        std::snprintf(name, sizeof name, "%06llu.log", static_cast<unsigned long long>(n));
        write_file(dir / name, w.out);
    }
}

std::map<std::string, std::string> all_entries(const LevelDbReader& r) {
    std::map<std::string, std::string> out;
    r.for_each([&](std::string_view k, std::string_view v) { out[std::string(k)] = std::string(v); });
    return out;
}

// ---- IndexedDB ----------------------------------------------------------------------------------------------------

std::string le_int(std::uint64_t v) {
    std::string out;
    do {
        out += static_cast<char>(v & 0xff);
        v >>= 8;
    } while (v);
    return out;
}

std::string key_prefix(std::uint64_t db, std::uint64_t store, std::uint64_t index) {
    const std::string a = le_int(db), b = le_int(store), c = le_int(index);
    const char first = static_cast<char>(((a.size() - 1) << 5) | ((b.size() - 1) << 2) | (c.size() - 1));
    return std::string(1, first) + a + b + c;
}

std::string utf16be(std::string_view ascii) {
    std::string out;
    for (const char ch : ascii) {
        out += '\0';
        out += ch;
    }
    return out;
}

std::string string_with_length(std::string_view ascii) { return varint(ascii.size()) + utf16be(ascii); }

std::string database_name_key(std::string_view origin, std::string_view name) {
    return key_prefix(0, 0, 0) + static_cast<char>(201) + string_with_length(origin) + string_with_length(name);
}

std::string store_name_key(std::uint64_t db, std::uint64_t store) {
    return key_prefix(db, 0, 0) + static_cast<char>(50) + varint(store) + '\0';
}

std::string data_key(std::uint64_t db, std::uint64_t store, std::string_view key) {
    return key_prefix(db, store, 1) + '\x01' + string_with_length(key);
}

// ---- V8 / Blink ---------------------------------------------------------------------------------------------------

const std::string kEnvelope = std::string("\xff\x15\xfe") + std::string(12, '\0') + "\xff\x0f";

std::string v8_str(std::string_view latin1) { return "\"" + varint(latin1.size()) + std::string(latin1); }
std::string v8_int(std::int32_t v) {
    return "I" + varint((static_cast<std::uint32_t>(v) << 1) ^ static_cast<std::uint32_t>(v >> 31));
}
std::string v8_double(double d) {
    std::uint64_t bits = 0;
    std::memcpy(&bits, &d, 8);
    return "N" + fixed64(bits);
}

// JSON -> V8 (subset): objects, arrays, strings (one-byte when Latin-1, else two-byte), numbers, booleans, null.
std::string v8_of(const json& j) {
    if (j.is_null()) return "0";
    if (j.is_boolean()) return j.get<bool>() ? "T" : "F";
    if (j.is_number_integer() && j.get<long long>() >= INT32_MIN && j.get<long long>() <= INT32_MAX)
        return v8_int(static_cast<std::int32_t>(j.get<long long>()));
    if (j.is_number()) return v8_double(j.get<double>());
    if (j.is_string()) {
        const std::string s = j.get<std::string>();
        bool ascii = true;
        for (const char ch : s) ascii = ascii && static_cast<unsigned char>(ch) < 0x80;
        if (ascii) return v8_str(s);
        return "S" + varint(s.size()) + s;
    }
    if (j.is_array()) {
        std::string out = "A" + varint(j.size());
        for (const auto& e : j) out += v8_of(e);
        return out + "$" + varint(0) + varint(j.size());
    }
    std::string out = "o";
    for (auto it = j.begin(); it != j.end(); ++it) out += v8_str(it.key()) + v8_of(it.value());
    return out + "{" + varint(j.size());
}

std::string idb_value(const json& j) { return varint(1) + kEnvelope + v8_of(j); }

}  // namespace

// ================================================================================================================

TEST(TeamsSnappy, LiteralAndCopies) {
    // "abcd" literal, copy-1 (len 4, off 4), copy-2 (len 2, off 2), copy-4 (len 3, off 1), 60-ext literal of 2 bytes.
    std::string in = varint(4 + 4 + 2 + 3 + 2);
    in += static_cast<char>(3 << 2);
    in += "abcd";
    in += static_cast<char>(((4 - 4) << 2) | 1);
    in += '\x04';
    in += static_cast<char>(((2 - 1) << 2) | 2);
    in += std::string("\x02\x00", 2);
    in += static_cast<char>(((3 - 1) << 2) | 3);
    in += fixed32(1);
    in += static_cast<char>(60 << 2);
    in += '\x01';
    in += "XY";
    const auto out = snappy_decompress(in);
    ASSERT_TRUE(out.ok()) << out.error().message;
    EXPECT_EQ(*out, "abcdabcdcddddXY");
}

TEST(TeamsSnappy, RoundTripAndErrors) {
    std::string text;
    for (int i = 0; i < 500; ++i) text += "hello teams cache " + std::to_string(i % 7);
    const auto out = snappy_decompress(snappy_compress(text));
    ASSERT_TRUE(out.ok());
    EXPECT_EQ(*out, text);
    EXPECT_LT(snappy_compress(text).size(), text.size() / 2);

    EXPECT_FALSE(snappy_decompress(varint(4) + std::string(1, 1) + '\0').ok());          // copy before any output
    EXPECT_FALSE(snappy_decompress(varint(5) + std::string(1, 0) + "a").ok());          // shorter than declared
    EXPECT_FALSE(snappy_decompress(varint(1) + std::string(1, 1 << 2) + "ab").ok());    // longer than declared
    EXPECT_EQ(snappy_decompress(varint(100), 10).error().code, "too_large");
    EXPECT_FALSE(snappy_decompress("").ok());
}

TEST(TeamsLevelDb, LogFragmentsAcrossBlocks) {
    LogWriter w;
    const std::string big(40000, 'x');
    std::string huge(100000, '\0');
    for (std::size_t i = 0; i < huge.size(); ++i) huge[i] = static_cast<char>(i * 7);
    w.add("first");
    w.add(big);
    w.add(huge);
    w.add("last");
    std::vector<std::string> got;
    int bad = 0;
    read_log(w.out, [&](std::string_view r) { got.emplace_back(r); }, &bad);
    ASSERT_EQ(got.size(), 4u);
    EXPECT_EQ(got[1], big);
    EXPECT_EQ(got[2], huge);
    EXPECT_EQ(got[3], "last");
    EXPECT_EQ(bad, 0);

    // A corrupted byte in the first record: it is skipped and counted, the others survive.
    std::string broken = w.out;
    broken[8] ^= 0x40;
    got.clear();
    read_log(broken, [&](std::string_view r) { got.emplace_back(r); }, &bad);
    EXPECT_EQ(bad, 1);
    ASSERT_EQ(got.size(), 3u);
    EXPECT_EQ(got[0], big);
}

TEST(TeamsLevelDb, TableReadsUncompressedAndSnappy) {
    const std::vector<std::pair<std::string, std::string>> entries = {
        {ikey("apple", 1), "red"}, {ikey("apricot", 2), "orange"}, {ikey("banana", 3), std::string(300, 'y')}};
    for (const bool snappy : {false, true}) {
        std::vector<std::pair<std::string, std::string>> got;
        int bad = 0;
        ASSERT_TRUE(read_table(build_table(entries, snappy), [&](std::string_view k, std::string_view v) {
            got.emplace_back(std::string(k), std::string(v));
        }, &bad));
        EXPECT_EQ(got, entries);
        EXPECT_EQ(bad, 0);
    }
    // A corrupted data block is skipped and counted; the table still opens.
    std::string t = build_table(entries, false);
    t[3] ^= 1;
    int bad = 0;
    std::size_t n = 0;
    EXPECT_TRUE(read_table(t, [&](std::string_view, std::string_view) { ++n; }, &bad));
    EXPECT_EQ(bad, 1);
    EXPECT_EQ(n, 1u);
}

TEST(TeamsLevelDb, ManifestLogsAndSequenceMerge) {
    const fs::path dir = fresh_dir("merge");
    Edit e1;
    e1.comparator = "idb_cmp1";
    e1.log_number = 3;
    e1.added = {4, 5};
    Edit e2;
    e2.deleted = {4};
    e2.added = {6};
    e2.log_number = 8;
    e2.next_file = 9;
    e2.last_seq = 7;
    const std::string big(40000, 'q');
    write_db(dir,
             {{4, {{ikey("z", 1), "obsolete"}}},
              {5, {{ikey("a", 1), "a1"}, {ikey("b", 2), "b1"}, {ikey("d", 3), "d1"}}},
              {6, {{ikey("b", 5), "b2"}, {ikey("e", 6, true), ""}, {ikey("f", 7), "f1"}}}},
             {e1, e2},
             {{7, {write_batch(1, {{false, "y", "stale log"}})}},
              {8, {write_batch(10, {{true, "a", ""}, {false, "c", "c1"}}), write_batch(12, {{false, "d", "d2"}}),
                   write_batch(13, {{false, "big", big}}), write_batch(14, {{true, "f", ""}})}}},
             /*snappy=*/true);
    write_file(dir / "LOCK", "");
    auto r = LevelDbReader::open(dir);
    ASSERT_TRUE(r.ok()) << r.error().message;
    const auto got = all_entries(*r);
    const std::map<std::string, std::string> want = {{"b", "b2"}, {"big", big}, {"c", "c1"}, {"d", "d2"}};
    EXPECT_EQ(got, want);
    const auto st = r->stats();
    EXPECT_EQ(st.tables, 2);
    EXPECT_EQ(st.logs, 1);
    EXPECT_EQ(st.missing_tables, 0);
    EXPECT_EQ(st.entries, 4u);
    EXPECT_EQ(st.deleted, 3u);  // a, e, f

    const auto m = parse_manifest(read_file(dir / "MANIFEST-000002"));
    ASSERT_TRUE(m.ok());
    EXPECT_EQ(m->comparator, "idb_cmp1");
    EXPECT_EQ(m->live_tables, (std::set<std::uint64_t>{5, 6}));
    EXPECT_EQ(m->log_number, 8u);

    EXPECT_EQ(LevelDbReader::open(dir / "missing").error().code, "not_found");
    fs::remove_all(dir);
}

TEST(TeamsLevelDb, WriteBatchRejectsBadCount) {
    std::string b = write_batch(5, {{false, "k", "v"}});
    b[8] = 2;  // count says 2, one record present
    int calls = 0;
    EXPECT_FALSE(decode_write_batch(b, [&](std::uint64_t, bool, std::string_view, std::string_view) { ++calls; }));
    EXPECT_EQ(calls, 0);
    EXPECT_EQ(manifest_name_from_current("MANIFEST-000012\r\n"), "MANIFEST-000012");
    EXPECT_EQ(manifest_name_from_current("../MANIFEST-1"), "");
    EXPECT_EQ(manifest_name_from_current("MANIFEST-12/x"), "");
}

TEST(TeamsIndexedDb, KeyPrefixAndKeys) {
    const auto p = decode_key_prefix(key_prefix(300, 2, 1) + "rest");
    ASSERT_TRUE(p.has_value());
    EXPECT_EQ(p->db_id, 300);
    EXPECT_EQ(p->store_id, 2);
    EXPECT_EQ(p->index_id, 1);
    EXPECT_EQ(p->size, 5u);
    EXPECT_FALSE(decode_key_prefix(std::string(1, '\x20') + "\x01").has_value());

    std::string_view k1("\x01\x02\x00h\x00i", 6);
    EXPECT_EQ(decode_idb_key(k1).value_or("?"), "hi");
    const std::string arr = std::string("\x04\x02", 2) + "\x03" + fixed64(0x4045000000000000ull) + std::string("\x06\x02\xab\xcd", 4);
    std::string_view k2 = arr;
    EXPECT_EQ(decode_idb_key(k2).value_or("?"), "[42,abcd]");
    std::string_view bad = "\x07";
    EXPECT_FALSE(decode_idb_key(bad).has_value());
}

TEST(TeamsIndexedDb, CatalogNamesAndRecords) {
    const fs::path dir = fresh_dir("catalog");
    Edit e;
    e.log_number = 1;
    e.added = {};
    std::vector<BatchOp> ops = {
        {false, database_name_key("https_teams.microsoft.com_0@1", "Teams:conversation-manager:x"), le_int(3)},
        {false, database_name_key("https_teams.microsoft.com_0@1", "other-db"), le_int(200)},
        {false, store_name_key(3, 1), utf16be("conversations")},
        {false, store_name_key(3, 2), utf16be("drafts")},
        {false, store_name_key(200, 1), utf16be("conversations")},
        {false, data_key(3, 1, "c1"), idb_value({{"id", "c1"}})},
        {false, data_key(3, 2, "d1"), idb_value({{"id", "d1"}})},
        {false, data_key(200, 1, "o1"), idb_value({{"id", "o1"}})},
    };
    write_db(dir, {}, {e}, {{1, {write_batch(1, ops)}}});
    auto r = LevelDbReader::open(dir);
    ASSERT_TRUE(r.ok());
    const auto cat = IdbCatalog::build(*r);
    ASSERT_EQ(cat.databases().size(), 2u);
    EXPECT_EQ(cat.database_name(3), "Teams:conversation-manager:x");
    EXPECT_EQ(cat.database_name(200), "other-db");
    EXPECT_EQ(cat.databases()[0].origin, "https_teams.microsoft.com_0@1");
    EXPECT_EQ(cat.stores().size(), 3u);
    std::vector<std::string> keys;
    cat.for_each_record("conversation-manager", "conversations", [&](const IdbRecord& rec) {
        keys.push_back(rec.key_text);
        const auto v = deserialize_idb_value(rec.value);
        ASSERT_TRUE(v.ok());
        EXPECT_EQ((*v)["id"], rec.key_text);
    });
    EXPECT_EQ(keys, std::vector<std::string>{"c1"});
    fs::remove_all(dir);
}

TEST(TeamsV8, ObjectsStringsNumbersRefsMaps) {
    std::string v = kEnvelope;
    v += "o";                                                      // id 0
    v += v8_str("name") + v8_str("Zo\xeb");                         // one-byte Latin-1
    v += v8_str("two") + std::string(1, '\0') + "c" + varint(8) + std::string("\xac\x20u\x00r\x00o\x00", 8);  // "€uro"
    v += v8_str("u8") + "S" + varint(2) + "\xc3\xbc";              // "ü"
    v += v8_str("n") + v8_int(-5);
    v += v8_str("u") + "U" + varint(4000000000u);
    v += v8_str("d") + v8_double(1.5);
    v += v8_str("arr") + "A" + varint(3) + v8_int(1) + v8_str("x") + "-" + "$" + varint(0) + varint(3);  // id 1
    v += v8_str("nested") + "o" + v8_str("a") + "o" + v8_str("b") + "T" + "{" + varint(1) + "{" + varint(1);  // ids 2, 3
    v += v8_str("again") + "^" + varint(2);
    v += v8_str("map") + ";" + v8_str("k") + v8_int(7) + v8_int(9) + "_" + ":" + varint(4);  // id 4
    v += v8_str("set") + "'" + v8_str("s1") + "0" + "," + varint(2);                        // id 5
    v += v8_str("when") + "D" + fixed64(0x4278ba2d5c600000ull);                            // id 6
    v += v8_int(12) + "F";                                                                   // numeric key
    v += v8_str("sparse") + "a" + varint(5) + v8_int(2) + v8_str("two") + "@" + varint(1) + varint(5);  // id 7
    v += "{" + varint(14);
    const auto j = deserialize_idb_value(v);
    ASSERT_TRUE(j.ok()) << j.error().code << " " << j.error().message;
    EXPECT_EQ((*j)["name"], "Zo\xc3\xab");
    EXPECT_EQ((*j)["two"], "\xe2\x82\xacuro");
    EXPECT_EQ((*j)["u8"], "\xc3\xbc");
    EXPECT_EQ((*j)["n"], -5);
    EXPECT_EQ((*j)["u"], 4000000000.0);
    EXPECT_EQ((*j)["d"], 1.5);
    EXPECT_EQ((*j)["arr"], json::parse(R"([1,"x",null])"));
    EXPECT_EQ((*j)["nested"], json::parse(R"({"a":{"b":true}})"));
    EXPECT_EQ((*j)["again"], (*j)["nested"]);
    EXPECT_EQ((*j)["map"], json::parse(R"({"k":7,"9":null})"));
    EXPECT_EQ((*j)["set"], json::parse(R"(["s1",null])"));
    EXPECT_GT((*j)["when"].get<double>(), 1e12);
    EXPECT_EQ((*j)["12"], false);
    EXPECT_EQ((*j)["sparse"], json::parse(R"([null,null,"two"])"));
}

TEST(TeamsV8, CyclesLimitsAndErrors) {
    // An object that references itself: the cycle becomes null.
    const std::string cyc = kEnvelope + "o" + v8_str("self") + "^" + varint(0) + "{" + varint(1);
    const auto j = deserialize_idb_value(cyc);
    ASSERT_TRUE(j.ok());
    EXPECT_EQ(*j, json::parse(R"({"self":null})"));

    std::string deep = kEnvelope;
    for (int i = 0; i < 100; ++i) deep += "A" + varint(1);
    EXPECT_EQ(deserialize_idb_value(deep).error().code, "too_deep");

    EXPECT_EQ(deserialize_idb_value(kEnvelope + "\\x").error().code, "unsupported");
    EXPECT_EQ(deserialize_idb_value(kEnvelope + "^" + varint(3)).error().code, "malformed");
    EXPECT_EQ(deserialize_idb_value(kEnvelope + "o" + v8_str("a")).error().code, "malformed");

    // Reference doubling: array i holds two references to array i-1, so expansion is 2^40 nodes; the budget cuts it.
    std::string flat = kEnvelope + "A" + varint(41) + "A" + varint(2) + "0" + "0" + "$" + varint(0) + varint(2);
    for (int i = 1; i <= 40; ++i)
        flat += "A" + varint(2) + "^" + varint(static_cast<std::uint64_t>(i)) + "^" + varint(static_cast<std::uint64_t>(i)) + "$" + varint(0) + varint(2);
    flat += "$" + varint(0) + varint(41);
    EXPECT_EQ(deserialize_idb_value(flat).error().code, "too_large");

    // Sparse arrays: an index past the declared length is dropped, and holes count against the limit.
    std::string sparse = kEnvelope + "a" + varint(0) + "U" + varint(65535) + "_" + "@" + varint(1) + varint(0);
    const auto empty = deserialize_idb_value(sparse);
    ASSERT_TRUE(empty.ok()) << empty.error().code;
    EXPECT_EQ(*empty, json::array());
    std::string holes = kEnvelope + "A" + varint(40);
    for (int i = 0; i < 40; ++i) holes += "a" + varint(65536) + "U" + varint(65535) + "_" + "@" + varint(1) + varint(65536);
    holes += "$" + varint(0) + varint(40);
    EXPECT_EQ(deserialize_idb_value(holes).error().code, "too_large");  // 40 x 65536 holes, from a few hundred bytes

    // Back-references repeat strings: the JSON's bytes are capped, not just its nodes.
    std::string big = kEnvelope + "A" + varint(200) + "o" + v8_str("k") + "\"" + varint(1 << 20) + std::string(1 << 20, 'x') + "{" + varint(1);
    for (int i = 1; i < 200; ++i) big += "^" + varint(1);
    big += "$" + varint(0) + varint(200);
    EXPECT_EQ(deserialize_idb_value(big).error().code, "too_large");
}

TEST(TeamsV8, SnappyWrappedAndBlob) {
    const std::string inner = kEnvelope + v8_of({{"id", "wrapped"}, {"pad", std::string(200, 'p')}});
    const std::string wrapped = std::string("\xff\x11\x02", 3) + snappy_compress(inner);
    const auto j = deserialize_idb_value(wrapped);
    ASSERT_TRUE(j.ok()) << j.error().message;
    EXPECT_EQ((*j)["id"], "wrapped");
    EXPECT_EQ(deserialize_idb_value(std::string("\xff\x11\x01\x05", 4)).error().code, "blob");
}

TEST(TeamsExtract, ConversationsMessagesProfiles) {
    const fs::path dir = fresh_dir("extract");
    const std::string tenant = "11111111-2222-3333-4444-555555555555";
    const std::string user = "AAAAAAAA-BBBB-CCCC-DDDD-EEEEEEEEEEEE";
    const std::string origin = "https_teams.microsoft.com_0@1";
    const json convs[] = {
        {{"id", "19:abc_def@unq.gbl.spaces"}, {"threadProperties", {{"threadType", "chat"}}}, {"lastMessageTimeUtc", 1700000000000LL}},
        {{"id", "19:xyz@thread.v2"}, {"threadProperties", {{"topic", "Project"}, {"threadType", "chat"}}}, {"lastMessageTimeUtc", "1700000001000"}},
        {{"id", "19:meeting_abc@thread.v2"}},
        {{"id", "19:chan@thread.tacv2"}, {"topic", "General"}},
        {{"conversationId", "19:pair@thread.v2"}, {"threadProperties", {{"threadType", "chat"}}}, {"members", json::array({1, 2})}},
        {{"noid", true}},
    };
    const json chain = {
        {"conversationId", "19:xyz@thread.v2"},
        {"replyChainId", "1700000000000"},
        {"messageMap",
         {{"m1", {{"id", "m1"}, {"originalArrivalTime", "2023-11-14T22:13:20.000Z"}, {"creator", "8:orgid:aaa"},
                  {"imDisplayName", "Alice"}, {"content", "<p>Hi <at id=\"0\">Bob</at> &amp; all</p><div>next&nbsp;line</div>"},
                  {"messageType", "RichText/Html"}, {"version", "1700000000000"},
                  {"properties", {{"mentions", R"([{"mri":"8:orgid:bbb","displayName":"Bob"}])"}}}}},
          {"m2", {{"clientMessageId", "m2"}, {"composeTime", 1700000002000LL}, {"creatorName", "Bob"},
                  {"from", "https://emea.ng.msg.teams.microsoft.com/v1/users/ME/contacts/8:orgid:bbb"},
                  {"content", " plain <text> "}, {"messageType", "Text"}, {"properties", {{"deletetime", 1700000003000LL}}}}},
          {"m3", {{"id", "m3"}, {"originalArrivalTime", 1700000004000LL}, {"messageType", "ThreadActivity/AddMember"},
                  {"content", "<addmember/>"}}},
          {"m4", {{"id", "m4"}}}}}};
    const json profile = {{"mri", "8:orgid:bbb"}, {"displayName", "Bob"}, {"userPrincipalName", "bob@example.com"}};

    std::vector<BatchOp> ops = {
        {false, database_name_key(origin, "Teams:conversation-manager:react-web-client:" + tenant + ":" + user + ":en-us"), le_int(1)},
        {false, database_name_key(origin, "Teams:replychain-manager:react-web-client:" + tenant + ":" + user + ":en-us"), le_int(2)},
        {false, database_name_key(origin, "Teams:profiles:react-web-client:" + tenant + ":" + user), le_int(3)},
        {false, store_name_key(1, 1), utf16be("conversations")},
        {false, store_name_key(2, 1), utf16be("replychains")},
        {false, store_name_key(3, 1), utf16be("profiles")},
        {false, data_key(2, 1, "chain1"), idb_value(chain)},
        {false, data_key(2, 1, "blob"), varint(1) + std::string("\xff\x11\x01\x00", 4)},
        {false, data_key(2, 1, "junk"), varint(1) + kEnvelope + "\\"},
        {false, data_key(3, 1, "p1"), idb_value(profile)},
        {false, data_key(3, 1, "p2"), idb_value({{"displayName", "No MRI"}})},
    };
    int i = 0;
    for (const auto& c : convs) ops.push_back({false, data_key(1, 1, "c" + std::to_string(i++)), idb_value(c)});
    Edit e;
    e.log_number = 1;
    write_db(dir, {}, {e}, {{1, {write_batch(1, ops)}}});
    auto r = LevelDbReader::open(dir);
    ASSERT_TRUE(r.ok());
    const auto ex = extract(IdbCatalog::build(*r));

    EXPECT_EQ(ex.user_mri, "8:orgid:aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee");
    std::map<std::string, TeamsConversation> conv;
    for (const auto& c : ex.conversations) conv[c.id] = c;
    ASSERT_EQ(conv.size(), 5u);
    EXPECT_EQ(conv["19:abc_def@unq.gbl.spaces"].type, "chat");
    EXPECT_EQ(conv["19:abc_def@unq.gbl.spaces"].last_activity_ms, 1700000000000LL);
    EXPECT_EQ(conv["19:xyz@thread.v2"].type, "group");
    EXPECT_EQ(conv["19:xyz@thread.v2"].topic, "Project");
    EXPECT_EQ(conv["19:xyz@thread.v2"].last_activity_ms, 1700000001000LL);
    EXPECT_EQ(conv["19:meeting_abc@thread.v2"].type, "meeting");
    EXPECT_EQ(conv["19:chan@thread.tacv2"].type, "channel");
    EXPECT_EQ(conv["19:chan@thread.tacv2"].topic, "General");
    EXPECT_EQ(conv["19:pair@thread.v2"].type, "chat");

    std::map<std::string, TeamsMessage> msg;
    for (const auto& m : ex.messages) msg[m.id] = m;
    ASSERT_EQ(msg.size(), 3u);
    const auto& m1 = msg["m1"];
    EXPECT_EQ(m1.conversation_id, "19:xyz@thread.v2");
    EXPECT_EQ(m1.at_ms, 1700000000000LL);
    EXPECT_EQ(m1.sender_mri, "8:orgid:aaa");
    EXPECT_EQ(m1.sender_name, "Alice");
    EXPECT_EQ(m1.text, "Hi @Bob & all\nnext line");
    EXPECT_EQ(m1.mention_mris, std::vector<std::string>{"8:orgid:bbb"});
    EXPECT_EQ(m1.version, "1700000000000");
    EXPECT_EQ(m1.parent_id, "1700000000000");
    EXPECT_FALSE(m1.deleted);
    EXPECT_FALSE(m1.system);
    const auto& m2 = msg["m2"];
    EXPECT_EQ(m2.sender_mri, "8:orgid:bbb");
    EXPECT_EQ(m2.sender_name, "Bob");
    EXPECT_EQ(m2.text, "plain <text>");
    EXPECT_TRUE(m2.deleted);
    EXPECT_EQ(m2.version.substr(0, 2), "h:");
    EXPECT_TRUE(msg["m3"].system);

    ASSERT_EQ(ex.profiles.size(), 1u);
    EXPECT_EQ(ex.profiles[0].mri, "8:orgid:bbb");
    EXPECT_EQ(ex.profiles[0].name, "Bob");
    EXPECT_EQ(ex.profiles[0].email, "bob@example.com");

    EXPECT_EQ(ex.blobs, 1);
    EXPECT_EQ(ex.decode_errors, 1);
    EXPECT_EQ(ex.skipped, 3);  // conversation without id, message without time, profile without MRI
    fs::remove_all(dir);
}

TEST(TeamsExtract, TimesAndTypes) {
    EXPECT_EQ(time_ms("2023-11-14T22:13:20Z"), 1700000000000LL);
    EXPECT_EQ(time_ms("2023-11-14T23:13:20.5+01:00"), 1700000000500LL);
    EXPECT_EQ(time_ms("1700000000000"), 1700000000000LL);
    EXPECT_EQ(time_ms(1700000000000LL), 1700000000000LL);
    EXPECT_EQ(time_ms("yesterday"), 0);
    EXPECT_EQ(time_ms("2023-13-14T22:13:20Z"), 0);
    EXPECT_EQ(conversation_type(json::object(), "48:notes"), "other");
    EXPECT_EQ(conversation_type({{"threadProperties", {{"productThreadType", "OneOnOneChat"}}}}, "19:x@thread.v2"), "chat");
    EXPECT_EQ(conversation_type({{"threadType", "space"}}, "19:x@thread.skype"), "channel");
    EXPECT_EQ(conversation_type(json::object(), "19:x@thread.skype"), "channel");
}

TEST(TeamsExtract, HtmlToText) {
    EXPECT_EQ(html_to_text("<div>Hello&nbsp;<b>world</b></div><p>Line&#33; &#x263A; &lt;ok&gt; &quot;q&quot; &#39;s&#39;</p>"),
              "Hello world\nLine! \xe2\x98\xba <ok> \"q\" 's'");
    EXPECT_EQ(html_to_text("a<br>b<br/>  c"), "a\nb\nc");
    EXPECT_EQ(html_to_text("<at id=\"1\">Jane Doe</at>, see this"), "@Jane Doe, see this");
    EXPECT_EQ(html_to_text("<style>p{color:red}</style>  text <SCRIPT>x</script>"), "text");
    EXPECT_EQ(html_to_text("5 < 6 &bogus; &"), "5 < 6 &bogus; &");
    EXPECT_EQ(html_to_text("   "), "");
}

TEST(TeamsSnapshot, CopiesLiveFilesAndReusesTables) {
    const fs::path src = fresh_dir("snap_src");
    const fs::path dst = fresh_dir("snap_dst");
    Edit e1;
    e1.log_number = 8;
    e1.added = {4, 5};
    Edit e2;
    e2.deleted = {4};
    write_db(src, {{4, {{ikey("z", 1), "old"}}}, {5, {{ikey("a", 1), "a1"}}}}, {e1, e2},
             {{8, {write_batch(10, {{false, "b", "b1"}})}}});
    write_file(src / "LOCK", "");
    write_file(dst / "stale.ldb", "x");

    auto s = snapshot_cache(src, dst);
    ASSERT_TRUE(s.ok()) << s.error().message;
    EXPECT_TRUE(fs::exists(dst / "000005.ldb"));
    EXPECT_FALSE(fs::exists(dst / "000004.ldb"));
    EXPECT_FALSE(fs::exists(dst / "LOCK"));
    EXPECT_FALSE(fs::exists(dst / "stale.ldb"));
    EXPECT_TRUE(fs::exists(dst / "000008.log"));
    auto r = LevelDbReader::open(dst);
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(all_entries(*r), (std::map<std::string, std::string>{{"a", "a1"}, {"b", "b1"}}));

    // A table already copied under the same name and size is not copied again.
    const std::string table = read_file(dst / "000005.ldb");
    write_file(dst / "000005.ldb", std::string(table.size(), 'Z'));
    ASSERT_TRUE(snapshot_cache(src, dst).ok());
    EXPECT_EQ(read_file(dst / "000005.ldb"), std::string(table.size(), 'Z'));

    EXPECT_EQ(snapshot_cache(src / "nope", dst).error().code, "not_found");
    fs::remove_all(src);
    fs::remove_all(dst);
}

TEST(TeamsSnapshot, FindsProfilesNewestFirst) {
    const fs::path root = fresh_dir("lad");
    const fs::path base = root / "Packages" / "MSTeams_8wekyb3d8bbwe" / "LocalCache" / "Microsoft" / "MSTeams" / "EBWebView";
    const fs::path a = base / "WV2Profile_tfw" / "IndexedDB" / "https_teams.microsoft.com_0.indexeddb.leveldb";
    const fs::path b = base / "WV2Profile_tfl" / "IndexedDB" / "https_teams.microsoft.com_0.indexeddb.leveldb";
    fs::create_directories(a);
    fs::create_directories(base / "Default" / "IndexedDB" / "https_teams.microsoft.com_0.indexeddb.leveldb");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    fs::create_directories(b);
    write_file(b / "CURRENT", "MANIFEST-000001\n");  // a later write: b is the most recently modified profile
    const auto found = find_teams_profiles(root);
    ASSERT_EQ(found.size(), 2u);
    EXPECT_EQ(found[0], b);
    EXPECT_EQ(found[1], a);
    EXPECT_TRUE(find_teams_profiles(root / "none").empty());
    fs::remove_all(root);
}

// Every truncation and many random corruptions of valid samples: decoders must return, never crash.
TEST(TeamsMalformed, TruncationsAndCorruptionsNeverCrash) {
    std::string text;
    for (int i = 0; i < 50; ++i) text += "abcabc" + std::to_string(i);
    const std::string snappy = snappy_compress(text);
    const std::string table = build_table({{ikey("k1", 1), "v1"}, {ikey("k2", 2), text}, {ikey("k3", 3), "v3"}}, true);
    LogWriter log;
    log.add(write_batch(1, {{false, "a", "b"}, {true, "c", ""}}));
    LogWriter manifest;
    Edit e;
    e.comparator = "idb_cmp1";
    e.log_number = 3;
    e.added = {5};
    manifest.add(version_edit(e));
    const std::string v8 = idb_value({{"id", "x"}, {"list", json::array({1, 2.5, "s", nullptr, true})},
                                      {"obj", {{"k", "\xc3\xa9t\xc3\xa9"}}}});
    const std::string wrapped = std::string("\xff\x11\x02", 3) + snappy_compress(v8.substr(1));
    const std::string block = build_block({{"key1", "value1"}, {"key2", "value2"}, {"key3", "v3"}});
    const std::string html = "<div>Hi <at>X</at> &amp; &#x1F600; <script>y</script></div>";
    const std::string idbkey = std::string("\x04\x02\x01\x01\x00", 5) + "a" + "\x06\x01\xff";

    auto run_all = [](std::string_view d) {
        (void)snappy_decompress(d);
        (void)read_table(d, [](std::string_view, std::string_view) {});
        (void)parse_block(d, [](std::string_view, std::string_view) {});
        read_log(d, [](std::string_view rec) {
            (void)decode_write_batch(rec, [](std::uint64_t, bool, std::string_view, std::string_view) {});
        });
        (void)parse_manifest(d);
        if (auto j = deserialize_idb_value(d)) (void)j->dump();
        (void)html_to_text(d);
        std::string_view k = d;
        (void)decode_idb_key(k);
        (void)decode_key_prefix(d);
    };
    const std::vector<std::string> samples = {snappy, table, log.out, manifest.out, v8.substr(1), wrapped, block, html, idbkey};
    std::mt19937 rng(12345);
    int runs = 0;
    for (const auto& s : samples) {
        for (std::size_t n = 0; n <= s.size(); ++n) {
            run_all(std::string_view(s).substr(0, n));
            ++runs;
        }
        for (int k = 0; k < 400; ++k) {
            std::string m = s;
            const int flips = 1 + static_cast<int>(rng() % 4);
            for (int f = 0; f < flips && !m.empty(); ++f) m[rng() % m.size()] = static_cast<char>(rng() & 0xff);
            run_all(m);
            ++runs;
        }
    }
    for (int k = 0; k < 2000; ++k) {
        std::string m(rng() % 64, '\0');
        for (auto& ch : m) ch = static_cast<char>(rng() & 0xff);
        run_all(m);
        ++runs;
    }
    EXPECT_GT(runs, 5000);
}
