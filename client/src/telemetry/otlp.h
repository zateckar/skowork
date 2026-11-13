// A minimal OTLP/HTTP protobuf encoder for log records (add-telemetry-and-admin design D1): an
// ExportLogsServiceRequest with one resource, one scope and the event records. The decoder reads the same subset back
// for the tests and the fuzz target.
#pragma once

#include "telemetry/record.h"

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace wb::telemetry {

struct ResourceInfo {
    std::vector<std::pair<Res, std::string>> attrs;
};

inline constexpr std::string_view kScopeName = "skowork.client";
inline constexpr std::string_view kOtlpContentType = "application/x-protobuf";

std::string encode_logs(const ResourceInfo& resource, std::span<const Record> records, std::string_view scope_version);

// What the decoder understands: the resource attributes and, per log record, the time, event name and attributes.
struct DecodedLog {
    unsigned long long time_unix_nano = 0;
    std::string event_name;
    std::vector<std::pair<std::string, Value>> attrs;
};
struct DecodedBatch {
    std::vector<std::pair<std::string, Value>> resource;
    std::string scope_name, scope_version;
    std::vector<DecodedLog> logs;
};
std::optional<DecodedBatch> decode_logs(std::string_view bytes);

}  // namespace wb::telemetry
