// client/telemetry (add-telemetry-and-admin design D1, D3): level handling from policy, the encrypted offline buffer
// (telemetry-buffer.db) with its size cap and discard counter, ordered batch upload in OTLP to the telemetry ingest,
// and the data behind the Telemetry viewer.
#pragma once

#include "base/json.h"
#include "base/result.h"
#include "base/time.h"
#include "platform/http.h"
#include "policy/core.h"
#include "storage/database.h"
#include "telemetry/otlp.h"
#include "telemetry/record.h"

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace wb::telemetry {

struct TelemetryEnv {
    // The policy in force (nullptr: none yet; the default level metadata applies).
    std::function<std::shared_ptr<const policy::EffectivePolicy>()> policy;
    // Posts one OTLP body to the ingest with the user's access token (empty function: not connected).
    std::function<Result<HttpResponse>(const std::string& body)> send;
    // Posts one crash dump with its metadata (id, process, exception code, module) to the ingest (empty: not connected).
    std::function<Result<HttpResponse>(const json& meta, const std::string& dump)> send_crash;
    std::function<std::string()> installation_id;
    std::string version, channel, os_build;
    const Clock* clock = &system_clock();
};

inline constexpr int kBatchRecords = 500;
inline constexpr long long kFlushIntervalMs = 60'000;
inline constexpr int kViewerDays = 30;
inline constexpr long long kCrashHoldMs = 3'600'000;           // a crash report waits an hour for the user's review
inline constexpr long long kMaxCrashBytes = 8LL * 1024 * 1024;  // larger dumps keep their summary only

class TelemetryService {
public:
    static Result<std::unique_ptr<TelemetryService>> open(const std::filesystem::path& file, const db_key& key, TelemetryEnv env);

    // Builds the record at the current level (nothing at off), redacts it, and stores it in the buffer. Thread-safe.
    void emit(Event e, std::span<const Field> fields);
    void emit(Event e, std::initializer_list<Field> fields) { emit(e, std::span<const Field>(fields.begin(), fields.size())); }

    [[nodiscard]] Level level() const;
    // The Group (or guardrail) that set the level, or "default".
    [[nodiscard]] std::string level_source() const;
    [[nodiscard]] bool content_active() const { return level() == Level::content; }

    // True when a batch should go now: 500 records are waiting, or 60 seconds passed since the last attempt.
    [[nodiscard]] bool flush_due() const;
    // Sends up to 500 buffered records in order and marks them sent once the ingest acknowledges them. Nothing is
    // sent at level off. Honours the ingest's retry-after.
    Status flush();

    // Crash Reports: whether a crash is captured (policy.telemetry.crash_reports, and the level is not off).
    [[nodiscard]] bool crash_reports_enabled() const;
    // Imports a dump written by a crashed process (deleting the file): its summary and the dump, held for review, and a
    // crash.report record. When crash reports are disabled the file is only deleted.
    void capture_crash(const std::filesystem::path& dump, const std::string& process);
    // The dumps the core left in dir when it crashed (imported at the next start).
    void import_pending(const std::filesystem::path& dir);
    Status delete_crash(const std::string& id);
    // The held dump of a crash report (empty once it was sent or when none was kept).
    [[nodiscard]] std::vector<std::uint8_t> crash_dump(const std::string& id) const;

    // The Telemetry viewer: level, source, counters, the records of the last 30 days (sent and buffered) and the crash
    // reports.
    [[nodiscard]] json view(int limit = 1000) const;

    [[nodiscard]] long long pending() const;
    [[nodiscard]] long long discarded() const { return discarded_.load(); }
    // Records dropped by the allowlist since start (a programming error; logged).
    [[nodiscard]] long long rejected_fields() const { return rejected_.load(); }

private:
    TelemetryService(std::unique_ptr<Database> db, TelemetryEnv env);
    void enforce_cap();
    void flush_crashes(long long now);
    [[nodiscard]] const Redactor& redactor() const;
    [[nodiscard]] long long cap_bytes() const;

    std::unique_ptr<Database> db_;
    TelemetryEnv env_;
    mutable std::mutex mx_;
    mutable long long redactor_version_ = -1;
    mutable std::unique_ptr<Redactor> redactor_;
    long long unsent_bytes_ = 0;
    long long unsent_count_ = 0;
    std::atomic<long long> discarded_{0};
    std::atomic<long long> rejected_{0};
    long long last_attempt_ms_ = 0;
    long long retry_at_ms_ = 0;
    std::mutex flush_mx_;
};

}  // namespace wb::telemetry
