// V8 ValueDeserializer subset plus the Blink and Chromium IndexedDB envelopes (client/teams;
// add-local-outlook-and-teams design D4). Values are first read into a flat node arena (object back-references point
// at nodes, so memory stays proportional to the input), then converted to JSON with references expanded under a node
// budget and cycles cut to null.
#include "teams/v8value.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "teams/bytes.h"
#include "teams/snappy.h"

namespace wb::teams {
namespace {

constexpr std::size_t kMaxSparseLength = 1u << 16;
// Array elements and object members together, including ones that point at the shared null: an arena of a million
// nodes stays small, a million holes per array does not.
constexpr std::size_t kMaxEntries = kV8MaxNodes;
// Bytes of strings and keys the JSON may hold after back-references are expanded.
constexpr std::size_t kMaxJsonBytes = 64u << 20;

struct Node {
    enum class Kind : std::uint8_t { null_value, boolean, number, string, object, array, ref };
    Kind kind = Kind::null_value;
    bool b = false;
    double num = 0;
    std::string str;
    std::vector<std::pair<std::string, std::size_t>> props;  // object members
    std::vector<std::size_t> items;                          // array elements
    std::size_t ref = 0;                                     // object id for Kind::ref
};

std::string number_key(double d) {
    if (std::isnan(d)) return "NaN";
    if (std::isinf(d)) return d > 0 ? "Infinity" : "-Infinity";
    char buf[40];
    if (d == std::floor(d) && std::fabs(d) < 1e15) std::snprintf(buf, sizeof buf, "%.0f", d);
    else std::snprintf(buf, sizeof buf, "%.17g", d);
    return buf;
}

class Deserializer {
public:
    explicit Deserializer(std::string_view data, std::uint32_t version) : c_(data), version_(version) {
        nodes_.push_back(Node{});  // node 0: the shared null
    }

    bool read(std::size_t& out) { return read_value(out, 0); }

    Result<json> to_json(std::size_t root) {
        visiting_.assign(nodes_.size(), false);
        budget_ = kV8MaxNodes;
        bytes_ = kMaxJsonBytes;
        json out;
        if (!convert(root, out, 0)) return make_error("too_large", "value expands beyond the node limit");
        return out;
    }

    Error error() const { return err_; }

private:
    bool fail(const char* code, const char* msg) {
        if (err_.code.empty()) err_ = make_error(code, msg);
        return false;
    }
    bool truncated() { return fail("malformed", "truncated or inconsistent value"); }

    bool new_node(Node n, std::size_t& out) {
        if (nodes_.size() >= kV8MaxNodes) return fail("too_large", "too many values");
        nodes_.push_back(std::move(n));
        out = nodes_.size() - 1;
        return true;
    }
    // Counts `n` more array elements or object members against kMaxEntries.
    bool add_entries(std::size_t n) {
        if (n > kMaxEntries - entries_) return fail("too_large", "too many values");
        entries_ += n;
        return true;
    }
    // Reserves an object id for a node (V8 numbers JS receivers in the order they are created).
    void assign_id(std::size_t node) { ids_.push_back(node); }

    void skip_padding() {
        std::uint8_t t = 0;
        while (c_.peek(t) && t == 0) c_.skip(1);
    }
    bool read_tag(std::uint8_t& tag) {
        skip_padding();
        return c_.u8(tag) || truncated();
    }
    bool peek_tag(std::uint8_t& tag) {
        skip_padding();
        return c_.peek(tag) || truncated();
    }
    bool varint32(std::uint32_t& v) { return c_.varint32(v) || truncated(); }

    bool read_string_body(std::uint8_t tag, std::string& out) {
        std::uint32_t len = 0;
        std::string_view raw;
        if (!varint32(len) || !c_.bytes(len, raw)) return truncated();
        if (tag == '"') out = latin1_to_utf8(raw);
        else if (tag == 'c') out = utf16_to_utf8(raw, /*big_endian=*/false);
        else out = sanitize_utf8(raw);
        return true;
    }
    bool read_string(std::string& out) {
        std::uint8_t tag = 0;
        if (!read_tag(tag)) return false;
        if (tag != '"' && tag != 'c' && tag != 'S') return fail("malformed", "expected a string");
        return read_string_body(tag, out);
    }

    bool read_bigint(Node& n) {
        std::uint32_t bitfield = 0;
        std::string_view digits;
        if (!varint32(bitfield) || !c_.bytes(bitfield >> 1, digits)) return truncated();
        if (digits.size() <= 8) {
            std::uint64_t v = 0;
            for (std::size_t i = 0; i < digits.size(); ++i) v |= static_cast<std::uint64_t>(static_cast<std::uint8_t>(digits[i])) << (8 * i);
            n.kind = Node::Kind::number;
            n.num = static_cast<double>(v) * ((bitfield & 1) ? -1.0 : 1.0);
        }
        return true;
    }

    std::string key_of(std::size_t idx) const {
        const Node& k = nodes_[idx];
        switch (k.kind) {
            case Node::Kind::string: return k.str;
            case Node::Kind::number: return number_key(k.num);
            case Node::Kind::boolean: return k.b ? "true" : "false";
            case Node::Kind::null_value: return "null";
            default: return "[object]";
        }
    }

    // Key/value pairs until `end_tag`; then the trailing property count varint (and `extra` more varints).
    bool read_properties(std::size_t node, std::uint8_t end_tag, int extra, int depth, bool sparse, std::size_t sparse_length = 0) {
        for (;;) {
            std::uint8_t tag = 0;
            if (!peek_tag(tag)) return false;
            if (tag == end_tag) {
                c_.skip(1);
                std::uint32_t ignored = 0;
                for (int i = 0; i <= extra; ++i)
                    if (!varint32(ignored)) return false;
                return true;
            }
            std::size_t k = 0, v = 0;
            if (!read_value(k, depth + 1) || !read_value(v, depth + 1)) return false;
            Node& target = nodes_[node];
            if (target.kind == Node::Kind::object) {
                if (!add_entries(1)) return false;
                target.props.emplace_back(key_of(k), v);
            } else if (sparse && nodes_[k].kind == Node::Kind::number) {
                // Only indexes inside the declared length; the holes up to it count as entries.
                const double d = nodes_[k].num;
                const std::size_t limit = std::min(sparse_length, kMaxSparseLength);
                if (d >= 0 && d < static_cast<double>(limit) && d == std::floor(d)) {
                    const auto i = static_cast<std::size_t>(d);
                    if (target.items.size() <= i) {
                        if (!add_entries(i + 1 - target.items.size())) return false;
                        target.items.resize(i + 1, 0);
                    }
                    target.items[i] = v;
                }
            }
            // Named properties on arrays have no JSON form and are dropped.
        }
    }

    bool read_value(std::size_t& out, int depth) {
        if (depth > kV8MaxDepth) return fail("too_deep", "value nested too deeply");
        std::uint8_t tag = 0;
        if (!read_tag(tag)) return false;
        Node n;
        switch (tag) {
            case '?': {  // verify object count, then the value
                std::uint32_t ignored = 0;
                if (!varint32(ignored)) return false;
                return read_value(out, depth + 1);
            }
            case '_':  // undefined
            case '0':  // null
            case '-':  // the hole
                out = 0;
                return true;
            case 'T':
            case 'F':
                n.kind = Node::Kind::boolean;
                n.b = tag == 'T';
                return new_node(std::move(n), out);
            case 'I': {
                std::uint32_t z = 0;
                if (!varint32(z)) return false;
                const std::int64_t v = static_cast<std::int64_t>(z >> 1) ^ -static_cast<std::int64_t>(z & 1);
                n.kind = Node::Kind::number;
                n.num = static_cast<double>(v);
                return new_node(std::move(n), out);
            }
            case 'U': {
                std::uint32_t v = 0;
                if (!varint32(v)) return false;
                n.kind = Node::Kind::number;
                n.num = static_cast<double>(v);
                return new_node(std::move(n), out);
            }
            case 'N':
                n.kind = Node::Kind::number;
                if (!c_.f64(n.num)) return truncated();
                return new_node(std::move(n), out);
            case 'Z':
                if (!read_bigint(n)) return false;
                return new_node(std::move(n), out);
            case '"':
            case 'c':
            case 'S':
                n.kind = Node::Kind::string;
                if (!read_string_body(tag, n.str)) return false;
                return new_node(std::move(n), out);
            case '^': {
                std::uint32_t id = 0;
                if (!varint32(id)) return false;
                if (id >= ids_.size()) return fail("malformed", "object reference out of range");
                n.kind = Node::Kind::ref;
                n.ref = id;
                return new_node(std::move(n), out);
            }
            case 'o': {
                n.kind = Node::Kind::object;
                if (!new_node(std::move(n), out)) return false;
                assign_id(out);
                return read_properties(out, '{', 0, depth, false);
            }
            case 'A': {
                std::uint32_t length = 0;
                if (!varint32(length)) return false;
                if (length > c_.remaining()) return truncated();  // every element takes at least one byte
                n.kind = Node::Kind::array;
                if (!new_node(std::move(n), out)) return false;
                assign_id(out);
                if (!add_entries(length)) return false;
                nodes_[out].items.reserve(length);
                for (std::uint32_t i = 0; i < length; ++i) {
                    std::size_t e = 0;
                    if (!read_value(e, depth + 1)) return false;
                    nodes_[out].items.push_back(e);
                }
                return read_properties(out, '$', 1, depth, false);
            }
            case 'a': {
                std::uint32_t length = 0;
                if (!varint32(length)) return false;
                n.kind = Node::Kind::array;
                if (!new_node(std::move(n), out)) return false;
                assign_id(out);
                return read_properties(out, '@', 1, depth, true, length);
            }
            case 'D':
                n.kind = Node::Kind::number;
                if (!c_.f64(n.num)) return truncated();
                if (!new_node(std::move(n), out)) return false;
                assign_id(out);
                return true;
            case 'y':
            case 'x':
                n.kind = Node::Kind::boolean;
                n.b = tag == 'y';
                if (!new_node(std::move(n), out)) return false;
                assign_id(out);
                return true;
            case 'n':
                n.kind = Node::Kind::number;
                if (!c_.f64(n.num)) return truncated();
                if (!new_node(std::move(n), out)) return false;
                assign_id(out);
                return true;
            case 'z':
                if (!read_bigint(n) || !new_node(std::move(n), out)) return false;
                assign_id(out);
                return true;
            case 's':
                n.kind = Node::Kind::string;
                if (!read_string(n.str) || !new_node(std::move(n), out)) return false;
                assign_id(out);
                return true;
            case 'R': {
                n.kind = Node::Kind::string;
                std::uint32_t flags = 0;
                if (!read_string(n.str) || !varint32(flags) || !new_node(std::move(n), out)) return false;
                assign_id(out);
                return true;
            }
            case ';': {  // Map: key/value pairs until ':' and a length
                n.kind = Node::Kind::object;
                if (!new_node(std::move(n), out)) return false;
                assign_id(out);
                for (;;) {
                    std::uint8_t t = 0;
                    if (!peek_tag(t)) return false;
                    if (t == ':') {
                        c_.skip(1);
                        std::uint32_t ignored = 0;
                        return varint32(ignored);
                    }
                    std::size_t k = 0, v = 0;
                    if (!read_value(k, depth + 1) || !read_value(v, depth + 1) || !add_entries(1)) return false;
                    nodes_[out].props.emplace_back(key_of(k), v);
                }
            }
            case '\'': {  // Set: values until ',' and a length
                n.kind = Node::Kind::array;
                if (!new_node(std::move(n), out)) return false;
                assign_id(out);
                for (;;) {
                    std::uint8_t t = 0;
                    if (!peek_tag(t)) return false;
                    if (t == ',') {
                        c_.skip(1);
                        std::uint32_t ignored = 0;
                        return varint32(ignored);
                    }
                    std::size_t v = 0;
                    if (!read_value(v, depth + 1) || !add_entries(1)) return false;
                    nodes_[out].items.push_back(v);
                }
            }
            case 'B':
            case '~': {  // ArrayBuffer (resizable: also a max length), kept as null; an optional view follows
                std::uint32_t len = 0, max_len = 0;
                if (!varint32(len)) return false;
                if (tag == '~' && !varint32(max_len)) return false;
                if (!c_.skip(len)) return truncated();
                if (!new_node(std::move(n), out)) return false;
                assign_id(out);
                std::uint8_t t = 0;
                c_.peek(t);
                if (t == 'V') {
                    c_.skip(1);
                    std::uint8_t subtag = 0;
                    std::uint32_t offset = 0, length = 0, flags = 0;
                    if (!c_.u8(subtag) || !varint32(offset) || !varint32(length)) return truncated();
                    if (version_ >= 14 && !varint32(flags)) return false;
                    std::size_t view = 0;
                    if (!new_node(Node{}, view)) return false;
                    assign_id(view);
                    out = view;
                }
                return true;
            }
            default:
                return fail("unsupported", "unsupported value tag");
        }
    }

    bool convert(std::size_t idx, json& out, int depth) {
        if (budget_ == 0) return false;
        --budget_;
        if (idx >= nodes_.size()) {
            out = nullptr;
            return true;
        }
        const Node& n = nodes_[idx];
        if (n.kind == Node::Kind::ref) {
            const std::size_t target = n.ref < ids_.size() ? ids_[n.ref] : 0;
            if (target == 0 || visiting_[target] || depth > 2 * kV8MaxDepth) {  // a cycle or runaway nesting
                out = nullptr;
                return true;
            }
            return convert(target, out, depth + 1);
        }
        switch (n.kind) {
            case Node::Kind::null_value: out = nullptr; return true;
            case Node::Kind::boolean: out = n.b; return true;
            case Node::Kind::number: out = n.num; return true;
            case Node::Kind::string:
                // Strings count in bytes too: a back-reference repeats them.
                if (n.str.size() > bytes_) return false;
                bytes_ -= n.str.size();
                out = n.str;
                return true;
            default: break;
        }
        visiting_[idx] = true;
        bool ok = true;
        if (n.kind == Node::Kind::object) {
            out = json::object();
            for (const auto& [k, v] : n.props) {
                if (k.size() > bytes_) {
                    ok = false;
                    break;
                }
                bytes_ -= k.size();
                json child;
                if (!(ok = convert(v, child, depth + 1))) break;
                out[k] = std::move(child);
            }
        } else {
            out = json::array();
            for (const auto v : n.items) {
                json child;
                if (!(ok = convert(v, child, depth + 1))) break;
                out.push_back(std::move(child));
            }
        }
        visiting_[idx] = false;
        return ok;
    }

    Cursor c_;
    std::uint32_t version_ = 0;
    std::vector<Node> nodes_;
    std::vector<std::size_t> ids_;
    std::vector<bool> visiting_;
    std::size_t budget_ = 0;
    std::size_t bytes_ = 0;
    std::size_t entries_ = 0;
    Error err_;
};

Result<json> deserialize(std::string_view value, int wrap_depth) {
    const auto byte = [&](std::size_t i) { return static_cast<std::uint8_t>(value[i]); };
    if (value.size() >= 3 && byte(0) == 0xff && byte(1) == 0x11) {  // Chromium kRequiresProcessingSSVPseudoVersion
        if (byte(2) == 0x01) return make_error("blob", "value is kept in a blob file");
        if (byte(2) != 0x02 || wrap_depth > 0) return make_error("unsupported", "unknown value wrapping");
        auto raw = snappy_decompress(value.substr(3));
        if (!raw) return make_error("malformed", "bad Snappy-wrapped value");
        return deserialize(*raw, wrap_depth + 1);
    }
    Cursor c(value);
    std::uint32_t version = 0;
    for (int i = 0; i < 4; ++i) {
        std::uint8_t t = 0;
        if (!c.peek(t)) break;
        if (t == 0xff) {  // Blink, then V8, version tag
            c.skip(1);
            if (!c.varint32(version)) return make_error("malformed", "bad version tag");
        } else if (t == 0xfe) {  // Blink trailer offset tag: 8-byte offset, 4-byte size
            if (!c.skip(13)) return make_error("malformed", "bad trailer offset tag");
        } else {
            break;
        }
    }
    Deserializer d(c.rest(), version);
    std::size_t root = 0;
    if (!d.read(root)) return d.error();
    return d.to_json(root);
}

}  // namespace

Result<json> deserialize_idb_value(std::string_view value) { return deserialize(value, 0); }

}  // namespace wb::teams
