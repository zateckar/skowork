// Read-only LevelDB subset: log reader, WriteBatch, table reader, MANIFEST replay and a sequence-merged view
// (client/teams; add-local-outlook-and-teams design D4).
#include "teams/leveldb.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <optional>
#include <system_error>
#include <tuple>
#include <vector>

#include "teams/bytes.h"
#include "teams/snappy.h"

namespace wb::teams {
namespace {

constexpr std::uint64_t kTableMagic = 0xdb4775248b80fb57ull;
constexpr std::size_t kFooterSize = 48;
constexpr std::size_t kBlockTrailerSize = 5;
constexpr std::uintmax_t kMaxFileSize = std::uintmax_t{1} << 30;  // 1 GiB: far above any real table or log

const std::array<std::uint32_t, 256>& crc_table() {
    static const std::array<std::uint32_t, 256> table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? (c >> 1) ^ 0x82f63b78u : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    return table;
}

bool read_whole_file(const std::filesystem::path& p, std::string& out) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(p, ec);
    if (ec || size > kMaxFileSize) return false;
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    out.assign(static_cast<std::size_t>(size), '\0');
    if (size > 0 && !f.read(out.data(), static_cast<std::streamsize>(size))) return false;
    return true;
}

struct BlockHandle {
    std::uint64_t offset = 0;
    std::uint64_t size = 0;
};

bool read_handle(Cursor& c, BlockHandle& h) { return c.varint64(h.offset) && c.varint64(h.size); }

// The bytes of one block: a view into the file when uncompressed, else the decompressed copy in `owned`.
Result<std::string_view> read_block(std::string_view file, const BlockHandle& h, std::string& owned) {
    if (h.offset > file.size() || h.size > file.size() - h.offset ||
        kBlockTrailerSize > file.size() - h.offset - h.size)
        return make_error("corrupt", "block handle out of range");
    const auto off = static_cast<std::size_t>(h.offset);
    const auto size = static_cast<std::size_t>(h.size);
    const std::string_view data = file.substr(off, size);
    const auto type = static_cast<std::uint8_t>(file[off + size]);
    Cursor tc(file.substr(off + size + 1, 4));
    std::uint32_t stored = 0;
    tc.fixed32(stored);
    if (crc32c_unmask(stored) != crc32c(file.substr(off, size + 1))) return make_error("crc", "block checksum mismatch");
    if (type == 0) return data;
    if (type == 1) {
        auto d = snappy_decompress(data);
        if (!d) return d.error();
        owned = std::move(d.value());
        return std::string_view(owned);
    }
    return make_error("unsupported", "unknown block compression");
}

bool apply_edit(std::string_view record, Manifest& m) {
    Cursor c(record);
    std::string comparator;
    bool has_comparator = false;
    std::optional<std::uint64_t> log_number, prev_log, next_file, last_seq;
    std::vector<std::uint64_t> deleted, added;
    while (!c.empty()) {
        std::uint32_t tag = 0;
        if (!c.varint32(tag)) return false;
        std::uint64_t v = 0;
        std::uint32_t level = 0;
        std::string_view s;
        switch (tag) {
            case 1:
                if (!c.slice(s)) return false;
                comparator.assign(s);
                has_comparator = true;
                break;
            case 2:
                if (!c.varint64(v)) return false;
                log_number = v;
                break;
            case 3:
                if (!c.varint64(v)) return false;
                next_file = v;
                break;
            case 4:
                if (!c.varint64(v)) return false;
                last_seq = v;
                break;
            case 5:  // compact pointer: level, internal key
                if (!c.varint32(level) || !c.slice(s)) return false;
                break;
            case 6:
                if (!c.varint32(level) || !c.varint64(v)) return false;
                deleted.push_back(v);
                break;
            case 7: {
                std::uint64_t size = 0;
                std::string_view smallest, largest;
                if (!c.varint32(level) || !c.varint64(v) || !c.varint64(size) || !c.slice(smallest) || !c.slice(largest))
                    return false;
                added.push_back(v);
                break;
            }
            case 9:
                if (!c.varint64(v)) return false;
                prev_log = v;
                break;
            default:
                return false;
        }
    }
    if (has_comparator) m.comparator = std::move(comparator);
    if (log_number) m.log_number = *log_number;
    if (prev_log) m.prev_log_number = *prev_log;
    if (next_file) m.next_file = *next_file;
    if (last_seq) m.last_sequence = *last_seq;
    for (const auto n : deleted) m.live_tables.erase(n);  // LevelDB applies an edit's deletions before its additions
    for (const auto n : added) m.live_tables.insert(n);
    return true;
}

std::string file_number_name(std::uint64_t n, const char* ext) {
    std::string digits = std::to_string(n);
    if (digits.size() < 6) digits.insert(0, 6 - digits.size(), '0');
    return digits + ext;
}

// The number of a "<digits>.log" file name, or nullopt.
std::optional<std::uint64_t> log_file_number(const std::string& name) {
    if (name.size() < 5 || name.compare(name.size() - 4, 4, ".log") != 0) return std::nullopt;
    const std::string stem = name.substr(0, name.size() - 4);
    if (stem.empty() || stem.size() > 19) return std::nullopt;
    std::uint64_t n = 0;
    for (const char ch : stem) {
        if (ch < '0' || ch > '9') return std::nullopt;
        n = n * 10 + static_cast<std::uint64_t>(ch - '0');
    }
    return n;
}

}  // namespace

std::uint32_t crc32c(std::string_view data, std::uint32_t init) {
    const auto& t = crc_table();
    std::uint32_t c = init ^ 0xffffffffu;
    for (const char ch : data) c = t[(c ^ static_cast<std::uint8_t>(ch)) & 0xffu] ^ (c >> 8);
    return c ^ 0xffffffffu;
}

std::uint32_t crc32c_mask(std::uint32_t crc) { return ((crc >> 15) | (crc << 17)) + 0xa282ead8u; }

std::uint32_t crc32c_unmask(std::uint32_t masked) {
    const std::uint32_t rot = masked - 0xa282ead8u;
    return (rot >> 17) | (rot << 15);
}

void read_log(std::string_view file, const std::function<void(std::string_view record)>& fn, int* bad_records) {
    int bad = 0;
    std::string pending;
    bool in_fragment = false;
    std::size_t off = 0;
    while (off < file.size()) {
        const std::size_t block_end = std::min(file.size(), (off / kLogBlockSize + 1) * kLogBlockSize);
        if (block_end - off < 7) {  // trailer padding
            off = block_end;
            continue;
        }
        Cursor h(file.substr(off, 7));
        std::uint32_t stored = 0;
        std::uint8_t l0 = 0, l1 = 0, type = 0;
        h.fixed32(stored);
        h.u8(l0);
        h.u8(l1);
        h.u8(type);
        const std::size_t len = static_cast<std::size_t>(l0) | (static_cast<std::size_t>(l1) << 8);
        if (type == 0 && len == 0) {  // zeroed (preallocated) space: nothing more in this block
            off = block_end;
            continue;
        }
        if (len > block_end - off - 7) {
            ++bad;
            pending.clear();
            in_fragment = false;
            off = block_end;
            continue;
        }
        const std::string_view data = file.substr(off + 7, len);
        off += 7 + len;
        if (crc32c_unmask(stored) != crc32c(data, crc32c(file.substr(off - len - 1, 1)))) {
            ++bad;
            pending.clear();
            in_fragment = false;
            continue;
        }
        switch (type) {
            case 1:  // FULL
                if (in_fragment) ++bad;
                pending.clear();
                in_fragment = false;
                fn(data);
                break;
            case 2:  // FIRST
                if (in_fragment) ++bad;
                pending.assign(data);
                in_fragment = true;
                break;
            case 3:  // MIDDLE
                if (!in_fragment) {
                    ++bad;
                    break;
                }
                pending.append(data);
                break;
            case 4:  // LAST
                if (!in_fragment) {
                    ++bad;
                    break;
                }
                pending.append(data);
                fn(pending);
                pending.clear();
                in_fragment = false;
                break;
            default:
                ++bad;
                pending.clear();
                in_fragment = false;
                break;
        }
    }
    // A fragment left open at the end of the file is a write that never finished: not an error.
    if (bad_records) *bad_records += bad;
}

Status decode_write_batch(std::string_view record, const BatchFn& fn) {
    Cursor c(record);
    std::uint64_t seq = 0;
    std::uint32_t count = 0;
    if (!c.fixed64(seq) || !c.fixed32(count)) return make_error("corrupt", "write batch header");
    std::vector<std::tuple<bool, std::string_view, std::string_view>> ops;
    while (!c.empty()) {
        std::uint8_t tag = 0;
        c.u8(tag);
        std::string_view key, value;
        if (tag == 1) {
            if (!c.slice(key) || !c.slice(value)) return make_error("corrupt", "write batch value");
            ops.emplace_back(false, key, value);
        } else if (tag == 0) {
            if (!c.slice(key)) return make_error("corrupt", "write batch deletion");
            ops.emplace_back(true, key, std::string_view{});
        } else {
            return make_error("corrupt", "write batch tag");
        }
    }
    if (ops.size() != count) return make_error("corrupt", "write batch count");
    for (const auto& [del, key, value] : ops) fn(seq++, del, key, value);
    return {};
}

Status parse_block(std::string_view block, const std::function<void(std::string_view key, std::string_view value)>& fn) {
    if (block.size() < 4) return make_error("corrupt", "block too small");
    Cursor tail(block.substr(block.size() - 4));
    std::uint32_t restarts = 0;
    tail.fixed32(restarts);
    if (restarts > (block.size() - 4) / 4) return make_error("corrupt", "restart count");
    const std::size_t data_end = block.size() - 4 - 4 * static_cast<std::size_t>(restarts);
    Cursor c(block.substr(0, data_end));
    std::string key;
    while (!c.empty()) {
        std::uint32_t shared = 0, non_shared = 0, value_len = 0;
        std::string_view delta, value;
        if (!c.varint32(shared) || !c.varint32(non_shared) || !c.varint32(value_len))
            return make_error("corrupt", "block entry header");
        if (shared > key.size()) return make_error("corrupt", "block shared prefix");
        if (!c.bytes(non_shared, delta) || !c.bytes(value_len, value)) return make_error("corrupt", "block entry");
        key.resize(shared);
        key.append(delta);
        fn(key, value);
    }
    return {};
}

Status read_table(std::string_view file, const std::function<void(std::string_view internal_key, std::string_view value)>& fn,
                  int* bad_blocks) {
    if (file.size() < kFooterSize) return make_error("corrupt", "table too small");
    Cursor footer(file.substr(file.size() - kFooterSize));
    BlockHandle meta, index;
    if (!read_handle(footer, meta) || !read_handle(footer, index)) return make_error("corrupt", "table footer");
    Cursor magic_c(file.substr(file.size() - 8));
    std::uint64_t magic = 0;
    magic_c.fixed64(magic);
    if (magic != kTableMagic) return make_error("corrupt", "table magic");
    std::string index_owned;
    auto index_block = read_block(file, index, index_owned);
    if (!index_block) return index_block.error();
    std::vector<BlockHandle> handles;
    const Status s = parse_block(*index_block, [&](std::string_view, std::string_view v) {
        Cursor hc(v);
        BlockHandle h;
        if (read_handle(hc, h)) handles.push_back(h);
        else if (bad_blocks) ++*bad_blocks;
    });
    if (!s) return s;
    for (const auto& h : handles) {
        std::string owned;
        auto block = read_block(file, h, owned);
        if (!block || !parse_block(*block, fn)) {
            if (bad_blocks) ++*bad_blocks;
        }
    }
    return {};
}

Result<Manifest> parse_manifest(std::string_view file) {
    Manifest m;
    bool any = false;
    read_log(file, [&](std::string_view rec) {
        if (apply_edit(rec, m)) any = true;
        else ++m.bad_records;
    }, &m.bad_records);
    if (!any) return make_error("corrupt", "no usable version edit in the manifest");
    return m;
}

std::string manifest_name_from_current(std::string_view current) {
    while (!current.empty() && (current.back() == '\n' || current.back() == '\r')) current.remove_suffix(1);
    constexpr std::string_view prefix = "MANIFEST-";
    if (current.size() <= prefix.size() || current.substr(0, prefix.size()) != prefix || current.size() > 64) return {};
    for (const char ch : current.substr(prefix.size()))
        if (ch < '0' || ch > '9') return {};
    return std::string(current);
}

void LevelDbReader::apply(std::string_view user_key, std::uint64_t seq, bool deleted, std::string_view value) {
    const auto it = entries_.find(user_key);
    if (it == entries_.end()) {
        entries_.emplace(std::string(user_key), Entry{seq, deleted, deleted ? std::string{} : std::string(value)});
        return;
    }
    if (seq < it->second.seq) return;
    it->second.seq = seq;
    it->second.deleted = deleted;
    it->second.value = deleted ? std::string{} : std::string(value);
}

Result<LevelDbReader> LevelDbReader::open(const std::filesystem::path& dir) {
    std::string current;
    if (!read_whole_file(dir / "CURRENT", current)) return make_error("not_found", "no CURRENT file");
    const std::string manifest_name = manifest_name_from_current(current);
    if (manifest_name.empty()) return make_error("corrupt", "CURRENT does not name a manifest");
    std::string manifest_bytes;
    if (!read_whole_file(dir / manifest_name, manifest_bytes)) return make_error("not_found", "manifest missing");
    auto manifest = parse_manifest(manifest_bytes);
    if (!manifest) return manifest.error();
    manifest_bytes.clear();
    manifest_bytes.shrink_to_fit();

    LevelDbReader r;
    auto on_internal = [&r](std::string_view ikey, std::string_view value) {
        if (ikey.size() < 8) return;
        Cursor tc(ikey.substr(ikey.size() - 8));
        std::uint64_t trailer = 0;
        tc.fixed64(trailer);
        const auto type = static_cast<std::uint8_t>(trailer & 0xff);
        if (type > 1) return;
        r.apply(ikey.substr(0, ikey.size() - 8), trailer >> 8, type == 0, value);
    };
    for (const auto n : manifest->live_tables) {
        std::string bytes;
        if (!read_whole_file(dir / file_number_name(n, ".ldb"), bytes) &&
            !read_whole_file(dir / file_number_name(n, ".sst"), bytes)) {
            ++r.stats_.missing_tables;
            continue;
        }
        if (read_table(bytes, on_internal, &r.stats_.bad_blocks)) ++r.stats_.tables;
        else ++r.stats_.bad_blocks;
    }

    std::vector<std::pair<std::uint64_t, std::filesystem::path>> logs;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const auto u8 = it->path().filename().u8string();  // never throws for names outside the ANSI code page
        const auto n = log_file_number(std::string(u8.begin(), u8.end()));
        if (!n) continue;
        if (*n >= manifest->log_number || (manifest->prev_log_number != 0 && *n == manifest->prev_log_number))
            logs.emplace_back(*n, it->path());
    }
    std::sort(logs.begin(), logs.end());
    for (const auto& [n, path] : logs) {
        std::string bytes;
        if (!read_whole_file(path, bytes)) continue;
        ++r.stats_.logs;
        read_log(bytes, [&](std::string_view rec) {
            const Status s = decode_write_batch(rec, [&](std::uint64_t seq, bool del, std::string_view k, std::string_view v) {
                r.apply(k, seq, del, v);
            });
            if (!s) ++r.stats_.bad_batches;
        }, &r.stats_.bad_log_records);
    }
    for (const auto& [k, e] : r.entries_) {
        if (e.deleted) ++r.stats_.deleted;
        else ++r.stats_.entries;
    }
    return r;
}

void LevelDbReader::for_each(const std::function<void(std::string_view user_key, std::string_view value)>& fn) const {
    for (const auto& [k, e] : entries_)
        if (!e.deleted) fn(k, e.value);
}

}  // namespace wb::teams
