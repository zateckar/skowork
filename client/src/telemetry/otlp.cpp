#include "telemetry/otlp.h"

#include <bit>
#include <cstdint>
#include <cstring>

namespace wb::telemetry {

// Field numbers from opentelemetry/proto (collector/logs/v1, logs/v1, resource/v1, common/v1).
namespace {

enum Wire : std::uint32_t { varint = 0, i64 = 1, len = 2, i32 = 5 };

void put_varint(std::string& out, std::uint64_t v) {
    while (v >= 0x80) {
        out += static_cast<char>((v & 0x7F) | 0x80);
        v >>= 7;
    }
    out += static_cast<char>(v);
}
void put_tag(std::string& out, std::uint32_t field, Wire w) { put_varint(out, (static_cast<std::uint64_t>(field) << 3) | w); }
void put_bytes(std::string& out, std::uint32_t field, std::string_view b) {
    put_tag(out, field, len);
    put_varint(out, b.size());
    out.append(b);
}
void put_fixed64(std::string& out, std::uint32_t field, std::uint64_t v) {
    put_tag(out, field, i64);
    for (int i = 0; i < 8; ++i) out += static_cast<char>((v >> (8 * i)) & 0xFF);
}

// AnyValue { string_value = 1; bool_value = 2; int_value = 3; double_value = 4; }
std::string any_value(const Value& v) {
    std::string out;
    if (const auto* s = std::get_if<std::string>(&v)) put_bytes(out, 1, *s);
    else if (const auto* b = std::get_if<bool>(&v)) {
        put_tag(out, 2, varint);
        put_varint(out, *b ? 1 : 0);
    } else if (const auto* i = std::get_if<long long>(&v)) {
        put_tag(out, 3, varint);
        put_varint(out, static_cast<std::uint64_t>(*i));
    } else if (const auto* d = std::get_if<double>(&v)) {
        put_fixed64(out, 4, std::bit_cast<std::uint64_t>(*d));
    }
    return out;
}

// KeyValue { key = 1; value = 2; }
std::string key_value(std::string_view key, const Value& v) {
    std::string out;
    put_bytes(out, 1, key);
    put_bytes(out, 2, any_value(v));
    return out;
}

}  // namespace

std::string encode_logs(const ResourceInfo& resource, std::span<const Record> records, std::string_view scope_version) {
    // Resource { attributes = 1; }
    std::string res;
    for (const auto& [r, v] : resource.attrs) put_bytes(res, 1, key_value(def(r).name, v));
    // InstrumentationScope { name = 1; version = 2; }
    std::string scope;
    put_bytes(scope, 1, kScopeName);
    put_bytes(scope, 2, scope_version);
    // ScopeLogs { scope = 1; log_records = 2; }
    std::string scope_logs;
    put_bytes(scope_logs, 1, scope);
    for (const Record& rec : records) {
        // LogRecord { time_unix_nano = 1 (fixed64); severity_number = 2; attributes = 6; observed_time_unix_nano = 11;
        //             event_name = 12; }
        std::string lr;
        const std::uint64_t nanos = static_cast<std::uint64_t>(rec.time_ms) * 1000000ULL;
        put_fixed64(lr, 1, nanos);
        put_tag(lr, 2, varint);
        put_varint(lr, 9);  // SEVERITY_NUMBER_INFO
        for (const Field& f : rec.attrs) put_bytes(lr, 6, key_value(def(f.attr).name, f.value));
        put_fixed64(lr, 11, nanos);
        put_bytes(lr, 12, def(rec.event).name);
        put_bytes(scope_logs, 2, lr);
    }
    // ResourceLogs { resource = 1; scope_logs = 2; }
    std::string resource_logs;
    put_bytes(resource_logs, 1, res);
    put_bytes(resource_logs, 2, scope_logs);
    // ExportLogsServiceRequest { resource_logs = 1; }
    std::string out;
    put_bytes(out, 1, resource_logs);
    return out;
}

// ---- Decoder (tests, fuzzing) ----------------------------------------------------------------------------------------

namespace {

class Reader {
public:
    explicit Reader(std::string_view b) : b_(b) {}
    [[nodiscard]] bool done() const { return i_ >= b_.size(); }
    bool read_varint(std::uint64_t& v) {
        v = 0;
        for (int shift = 0; shift < 64; shift += 7) {
            if (i_ >= b_.size()) return false;
            const auto c = static_cast<unsigned char>(b_[i_++]);
            v |= static_cast<std::uint64_t>(c & 0x7F) << shift;
            if (!(c & 0x80)) return true;
        }
        return false;
    }
    bool fixed(int n, std::uint64_t& v) {
        if (b_.size() - i_ < static_cast<size_t>(n)) return false;
        v = 0;
        for (int k = 0; k < n; ++k) v |= static_cast<std::uint64_t>(static_cast<unsigned char>(b_[i_ + k])) << (8 * k);
        i_ += n;
        return true;
    }
    bool bytes(std::string_view& out) {
        std::uint64_t n = 0;
        if (!read_varint(n) || n > b_.size() - i_) return false;
        out = b_.substr(i_, n);
        i_ += n;
        return true;
    }
    // Reads the next field: its number, wire type, and the value (varint/fixed in `num`, length-delimited in `sub`).
    bool next(std::uint32_t& field, std::uint32_t& wire, std::uint64_t& num, std::string_view& sub) {
        std::uint64_t tag = 0;
        if (!read_varint(tag)) return false;
        field = static_cast<std::uint32_t>(tag >> 3);
        wire = static_cast<std::uint32_t>(tag & 7);
        if (field == 0) return false;
        switch (wire) {
        case varint: return read_varint(num);
        case i64: return fixed(8, num);
        case len: return bytes(sub);
        case i32: return fixed(4, num);
        default: return false;
        }
    }

private:
    std::string_view b_;
    size_t i_ = 0;
};

// Calls fn(field, wire, number, bytes) for each field of a message; false when the message is malformed or fn fails.
template <class Fn>
bool each_field(std::string_view b, Fn&& fn) {
    Reader rd(b);
    while (!rd.done()) {
        std::uint32_t field = 0, wire = 0;
        std::uint64_t num = 0;
        std::string_view sub;
        if (!rd.next(field, wire, num, sub) || !fn(field, wire, num, sub)) return false;
    }
    return true;
}

bool parse_any(std::string_view b, Value& out) {
    out = std::string();
    return each_field(b, [&](std::uint32_t field, std::uint32_t wire, std::uint64_t num, std::string_view sub) {
        if (field == 1 && wire == len) out = std::string(sub);
        else if (field == 2 && wire == varint) out = num != 0;
        else if (field == 3 && wire == varint) out = static_cast<long long>(num);
        else if (field == 4 && wire == i64) out = std::bit_cast<double>(num);
        return true;
    });
}

bool parse_kv(std::string_view b, std::pair<std::string, Value>& out) {
    return each_field(b, [&](std::uint32_t field, std::uint32_t wire, std::uint64_t, std::string_view sub) {
        if (field == 1 && wire == len) out.first = std::string(sub);
        else if (field == 2 && wire == len) return parse_any(sub, out.second);
        return true;
    });
}

bool parse_log(std::string_view b, DecodedLog& out) {
    return each_field(b, [&](std::uint32_t field, std::uint32_t wire, std::uint64_t num, std::string_view sub) {
        if (field == 1 && wire == i64) out.time_unix_nano = num;
        else if (field == 12 && wire == len) out.event_name = std::string(sub);
        else if (field == 6 && wire == len) {
            std::pair<std::string, Value> kv;
            if (!parse_kv(sub, kv)) return false;
            out.attrs.push_back(std::move(kv));
        }
        return true;
    });
}

bool parse_scope_logs(std::string_view b, DecodedBatch& out) {
    return each_field(b, [&](std::uint32_t field, std::uint32_t wire, std::uint64_t, std::string_view sub) {
        if (field == 1 && wire == len)
            return each_field(sub, [&](std::uint32_t f2, std::uint32_t w2, std::uint64_t, std::string_view v2) {
                if (f2 == 1 && w2 == len) out.scope_name = std::string(v2);
                if (f2 == 2 && w2 == len) out.scope_version = std::string(v2);
                return true;
            });
        if (field == 2 && wire == len) {
            DecodedLog l;
            if (!parse_log(sub, l)) return false;
            out.logs.push_back(std::move(l));
        }
        return true;
    });
}

bool parse_resource_logs(std::string_view b, DecodedBatch& out) {
    return each_field(b, [&](std::uint32_t field, std::uint32_t wire, std::uint64_t, std::string_view sub) {
        if (field == 1 && wire == len)
            return each_field(sub, [&](std::uint32_t f2, std::uint32_t w2, std::uint64_t, std::string_view v2) {
                if (f2 != 1 || w2 != len) return true;
                std::pair<std::string, Value> kv;
                if (!parse_kv(v2, kv)) return false;
                out.resource.push_back(std::move(kv));
                return true;
            });
        if (field == 2 && wire == len) return parse_scope_logs(sub, out);
        return true;
    });
}

bool parse_request(std::string_view b, DecodedBatch& out) {
    return each_field(b, [&](std::uint32_t field, std::uint32_t wire, std::uint64_t, std::string_view sub) {
        return field == 1 && wire == len ? parse_resource_logs(sub, out) : true;
    });
}

}  // namespace

std::optional<DecodedBatch> decode_logs(std::string_view bytes) {
    DecodedBatch b;
    if (!parse_request(bytes, b)) return std::nullopt;
    return b;
}

}  // namespace wb::telemetry
