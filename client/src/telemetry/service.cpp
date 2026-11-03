#include "telemetry/service.h"

#include "base/strings.h"
#include "platform/crash.h"
#include "platform/crypto.h"
#include "platform/log.h"

#include <algorithm>
#include <fstream>
#include <vector>

namespace wb::telemetry {
namespace {

constexpr const char* kLog = "telemetry";

// telemetry-buffer.db (design D3): one row per record; sent rows stay 30 days for the Telemetry viewer.
const Migration kMigrations[] = {
    {601, R"(
        CREATE TABLE records (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            at INTEGER NOT NULL,
            body TEXT NOT NULL,
            size INTEGER NOT NULL,
            sent_at INTEGER
        );
        CREATE INDEX records_unsent ON records(sent_at, id);
        CREATE TABLE meta (k TEXT PRIMARY KEY, v INTEGER NOT NULL);
    )"},
    // Crash Reports (design D4): the summary stays for the viewer; the dump is dropped once sent.
    {602, R"(
        CREATE TABLE crashes (
            id TEXT PRIMARY KEY,
            at INTEGER NOT NULL,
            process TEXT NOT NULL,
            summary TEXT NOT NULL,
            size INTEGER NOT NULL,
            dump BLOB,
            sent_at INTEGER
        );
    )"},
};

long long retry_after_ms(const HttpResponse& r) {
    const std::string v = r.header("retry-after");
    long long s = 0;
    for (const char c : v) {
        if (c < '0' || c > '9') break;
        s = s * 10 + (c - '0');
        if (s > 86'400) break;
    }
    return std::clamp(s, 1LL, 86'400LL) * 1000;
}

}  // namespace

Result<std::unique_ptr<TelemetryService>> TelemetryService::open(const std::filesystem::path& file, const db_key& key, TelemetryEnv env) {
    MigrationOutcome outcome;
    auto db = open_and_migrate(file, key, kMigrations, outcome);
    if (!db) return db.error();
    if (outcome.degraded) return make_error("storage", "telemetry buffer migration failed: " + outcome.failure);
    auto svc = std::unique_ptr<TelemetryService>(new TelemetryService(std::move(*db), std::move(env)));
    return svc;
}

TelemetryService::TelemetryService(std::unique_ptr<Database> db, TelemetryEnv env) : db_(std::move(db)), env_(std::move(env)) {
    auto st = db_->prepare("SELECT COUNT(*), COALESCE(SUM(size), 0) FROM records WHERE sent_at IS NULL");
    if (st.step()) {
        unsent_count_ = st.col_int(0);
        unsent_bytes_ = st.col_int(1);
    }
    auto m = db_->prepare("SELECT v FROM meta WHERE k = 'discarded'");
    if (m.step()) discarded_ = m.col_int(0);
}

Level TelemetryService::level() const {
    const auto p = env_.policy ? env_.policy() : nullptr;
    if (!p) return Level::metadata;
    return parse_level(p->string("telemetry.level", "metadata")).value_or(Level::metadata);
}

std::string TelemetryService::level_source() const {
    const auto p = env_.policy ? env_.policy() : nullptr;
    return p ? p->source("telemetry.level") : std::string("default");
}

long long TelemetryService::cap_bytes() const {
    const auto p = env_.policy ? env_.policy() : nullptr;
    const long long mb = p ? p->integer("telemetry.buffer_max_mb", 20) : 20;
    return std::max(1LL, mb) * 1024 * 1024;
}

const Redactor& TelemetryService::redactor() const {
    // Called with mx_ held. Rebuilt when the policy version changes.
    const auto p = env_.policy ? env_.policy() : nullptr;
    const long long v = p ? p->version() : 0;
    if (!redactor_ || v != redactor_version_) {
        redactor_ = std::make_unique<Redactor>(p ? p->strings("telemetry.redaction_patterns") : std::vector<std::string>{});
        redactor_version_ = v;
        for (const auto& bad : redactor_->invalid()) log::warn(kLog, "ignored redaction pattern that does not compile (" + std::to_string(bad.size()) + " chars)");
    }
    return *redactor_;
}

void TelemetryService::emit(Event e, std::span<const Field> fields) {
    const Level lv = level();
    if (lv == Level::off) return;
    const long long now = env_.clock->unix_millis();
    const std::lock_guard lock(mx_);
    FilterStats st;
    const auto rec = make_record(e, now, fields, lv, redactor(), &st);
    if (!rec) return;
    if (st.not_in_event + st.wrong_type > 0) {
        rejected_ += st.not_in_event + st.wrong_type;
        log::warn(kLog, std::string("dropped fields that the catalogue does not allow on ") + def(e).name);
    }
    const std::string body = to_json(*rec).dump();
    auto ins = db_->prepare("INSERT INTO records(at, body, size) VALUES(?1, ?2, ?3)");
    ins.bind(1, now).bind(2, body).bind(3, static_cast<long long>(body.size()));
    if (!ins.run()) return;
    ++unsent_count_;
    unsent_bytes_ += static_cast<long long>(body.size());
    enforce_cap();
}

void TelemetryService::enforce_cap() {
    // Called with mx_ held. Oldest unsent records go first (Offline Telemetry Buffer: Buffer full).
    const long long cap = cap_bytes();
    if (unsent_bytes_ <= cap) return;
    long long dropped = 0;
    auto sel = db_->prepare("SELECT id, size FROM records WHERE sent_at IS NULL ORDER BY id LIMIT 256");
    std::vector<std::pair<long long, long long>> victims;
    while (unsent_bytes_ > cap) {
        victims.clear();
        sel.reset();
        while (sel.step()) victims.emplace_back(sel.col_int(0), sel.col_int(1));
        if (victims.empty()) break;
        auto del = db_->prepare("DELETE FROM records WHERE id = ?1");
        for (const auto& [id, size] : victims) {
            if (unsent_bytes_ <= cap) break;
            del.reset();
            del.bind(1, id);
            (void)del.run();
            unsent_bytes_ -= size;
            --unsent_count_;
            ++dropped;
        }
    }
    if (dropped > 0) {
        discarded_ += dropped;
        auto m = db_->prepare("INSERT INTO meta(k, v) VALUES('discarded', ?1) ON CONFLICT(k) DO UPDATE SET v = excluded.v");
        m.bind(1, discarded_.load());
        (void)m.run();
    }
}

long long TelemetryService::pending() const {
    const std::lock_guard lock(mx_);
    return unsent_count_;
}

bool TelemetryService::flush_due() const {
    const long long now = env_.clock->unix_millis();
    const std::lock_guard lock(mx_);
    if (unsent_count_ == 0 && discarded_ == 0) return false;
    if (now < retry_at_ms_) return false;
    return unsent_count_ >= kBatchRecords || now - last_attempt_ms_ >= kFlushIntervalMs;
}

Status TelemetryService::flush() {
    const std::lock_guard flush_lock(flush_mx_);
    const Level lv = level();
    const long long now = env_.clock->unix_millis();
    flush_crashes(now);
    std::vector<long long> ids;
    std::vector<Record> recs;
    long long reported = 0;
    {
        const std::lock_guard lock(mx_);
        last_attempt_ms_ = now;
        if (lv == Level::off) return {};  // Telemetry Levels: nothing is sent at off
        if (now < retry_at_ms_) return make_error("retry_after", "the ingest asked to wait");
        if (!env_.send) return make_error("offline", "not connected to the telemetry ingest");
        // Records dropped by the cap since the last batch are reported first in this batch.
        if (const long long n = discarded_.load(); n > 0) {
            const Field f{Attr::telemetry_discarded, n};
            if (auto r = make_record(Event::telemetry_buffer, now, std::span<const Field>(&f, 1), lv, redactor())) {
                recs.push_back(std::move(*r));
                reported = n;
            }
        }
        auto st = db_->prepare("SELECT id, body FROM records WHERE sent_at IS NULL ORDER BY id LIMIT ?1");
        st.bind(1, kBatchRecords);
        while (st.step()) {
            // Filtered again at the level in force now: content kept from an earlier content level is not sent
            // once the level is metadata.
            const auto parsed = parse_json(st.col_text(1));
            const auto stored = parsed ? record_from_json(*parsed) : std::nullopt;
            ids.push_back(st.col_int(0));
            if (!stored) continue;
            if (auto r = make_record(stored->event, stored->time_ms, stored->attrs, lv, redactor())) recs.push_back(std::move(*r));
        }
    }
    if (ids.empty() && reported == 0) return {};

    ResourceInfo res;
    res.attrs = {{Res::service_version, env_.version}, {Res::workbench_channel, env_.channel}, {Res::os_build, env_.os_build},
                 {Res::workbench_installation_id, env_.installation_id ? env_.installation_id() : std::string{}}};
    const std::string body = encode_logs(res, recs, env_.version);
    auto sent = env_.send(body);
    const std::lock_guard lock(mx_);
    if (!sent) {
        retry_at_ms_ = now + kFlushIntervalMs;
        return sent.error();
    }
    if (sent->status == 429 || sent->status == 503) {
        retry_at_ms_ = now + retry_after_ms(*sent);
        return make_error("retry_after", "the telemetry ingest asked to retry later");
    }
    if (sent->status < 200 || sent->status >= 300) {
        retry_at_ms_ = now + kFlushIntervalMs;
        return make_error("http", "telemetry ingest answered " + std::to_string(sent->status));
    }
    retry_at_ms_ = 0;
    const Status s = db_->transaction([&]() -> Status {
        auto up = db_->prepare("UPDATE records SET sent_at = ?1 WHERE id = ?2 AND sent_at IS NULL");
        for (const long long id : ids) {
            up.reset();
            up.bind(1, now).bind(2, id);
            if (Status r = up.run(); !r) return r;
        }
        return {};
    });
    if (!s) return s;
    if (reported > 0) {
        // Delivered: the counter starts again (records dropped meanwhile stay counted), and the viewer lists the record.
        discarded_ -= reported;
        auto m = db_->prepare("UPDATE meta SET v = ?1 WHERE k = 'discarded'");
        m.bind(1, discarded_.load());
        (void)m.run();
        const std::string rb = to_json(recs.front()).dump();
        auto ins = db_->prepare("INSERT INTO records(at, body, size, sent_at) VALUES(?1, ?2, ?3, ?1)");
        ins.bind(1, now).bind(2, rb).bind(3, static_cast<long long>(rb.size()));
        (void)ins.run();
    }
    auto st = db_->prepare("SELECT COUNT(*), COALESCE(SUM(size), 0) FROM records WHERE sent_at IS NULL");
    if (st.step()) {
        unsent_count_ = st.col_int(0);
        unsent_bytes_ = st.col_int(1);
    }
    // Sent records stay for the viewer's 30 days.
    auto prune = db_->prepare("DELETE FROM records WHERE sent_at IS NOT NULL AND at < ?1");
    prune.bind(1, now - static_cast<long long>(kViewerDays) * 86'400'000);
    (void)prune.run();
    auto prune_crashes = db_->prepare("DELETE FROM crashes WHERE at < ?1");
    prune_crashes.bind(1, now - static_cast<long long>(kViewerDays) * 86'400'000);
    (void)prune_crashes.run();
    return {};
}

// ---- Crash Reports ---------------------------------------------------------------------------------------------------

bool TelemetryService::crash_reports_enabled() const {
    if (level() == Level::off) return false;
    const auto p = env_.policy ? env_.policy() : nullptr;
    const json* v = p ? p->find("telemetry.crash_reports") : nullptr;
    return !(v && v->is_boolean() && !v->get<bool>());
}

void TelemetryService::capture_crash(const std::filesystem::path& dump, const std::string& process) {
    std::error_code ec;
    if (!crash_reports_enabled()) {
        std::filesystem::remove(dump, ec);
        return;
    }
    const auto summary = crash::summarize(dump);
    const auto size = static_cast<long long>(std::filesystem::file_size(dump, ec));
    std::vector<std::uint8_t> bytes;
    if (!ec && size > 0 && size <= kMaxCrashBytes) {
        std::ifstream in(dump, std::ios::binary);
        bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    std::filesystem::remove(dump, ec);
    json s = summary ? json{{"exception_code", summary->exception_code}, {"module", summary->module}, {"threads", summary->threads},
                            {"modules", summary->modules}}
                     : json{{"exception_code", ""}, {"module", ""}, {"error", summary.error().message}};
    const std::string id = random_urlsafe(12);
    const long long now = env_.clock->unix_millis();
    {
        const std::lock_guard lock(mx_);
        auto ins = db_->prepare("INSERT INTO crashes(id, at, process, summary, size, dump) VALUES(?1, ?2, ?3, ?4, ?5, ?6)");
        ins.bind(1, id).bind(2, now).bind(3, process).bind(4, s.dump()).bind(5, size);
        if (bytes.empty()) ins.bind_null(6);
        else ins.bind_blob(6, bytes);
        if (Status r = ins.run(); !r) {
            log::warn(kLog, "could not store a crash report: " + r.error().message);
            return;
        }
    }
    log::warn(kLog, "captured a crash report of " + process + " (" + jstr(s, "exception_code") + " in " + jstr(s, "module") + ")");
    emit(Event::crash_report, {{Attr::crash_id, id}, {Attr::crash_process, process}, {Attr::crash_exception_code, jstr(s, "exception_code")},
                               {Attr::crash_module, jstr(s, "module")}, {Attr::crash_dump_bytes, size}});
}

void TelemetryService::import_pending(const std::filesystem::path& dir) {
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        if (!e.is_regular_file() || e.path().extension() != L".dmp") continue;
        // core-<ms>.dmp from the core's own handler; worker dumps are imported as their worker ends, so one left here
        // (worker-<role>-...) belongs to a worker whose core went first.
        const std::string name = e.path().stem().string();
        std::string process = "core";
        if (name.starts_with("worker-")) process = name.substr(0, name.find('-', 7));
        if (std::filesystem::file_size(e.path(), ec) == 0) std::filesystem::remove(e.path(), ec);
        else capture_crash(e.path(), process);
    }
}

Status TelemetryService::delete_crash(const std::string& id) {
    const std::lock_guard lock(mx_);
    auto del = db_->prepare("DELETE FROM crashes WHERE id = ?1");
    del.bind(1, id);
    return del.run();
}

void TelemetryService::flush_crashes(long long now) {
    // Called from flush() with flush_mx_ held. A report goes once its hour of review has passed.
    if (!env_.send_crash || !crash_reports_enabled()) return;
    struct Pending {
        std::string id, process;
        json summary;
        std::vector<std::uint8_t> dump;
    };
    std::vector<Pending> todo;
    {
        const std::lock_guard lock(mx_);
        if (now < retry_at_ms_) return;
        auto st = db_->prepare("SELECT id, process, summary, dump FROM crashes WHERE sent_at IS NULL AND dump IS NOT NULL AND at <= ?1 ORDER BY at LIMIT 5");
        st.bind(1, now - kCrashHoldMs);
        while (st.step()) {
            auto s = parse_json(st.col_text(2));
            todo.push_back({st.col_text(0), st.col_text(1), s ? *s : json::object(), st.col_blob(3)});
        }
    }
    for (const auto& p : todo) {
        const json meta = {{"id", p.id}, {"process", p.process}, {"exception_code", jstr(p.summary, "exception_code")},
                           {"module", jstr(p.summary, "module")},
                           {"installation_id", env_.installation_id ? env_.installation_id() : std::string{}}};
        auto sent = env_.send_crash(meta, std::string(p.dump.begin(), p.dump.end()));
        const std::lock_guard lock(mx_);
        if (!sent || sent->status == 429 || sent->status == 503) {
            retry_at_ms_ = now + (sent ? retry_after_ms(*sent) : kFlushIntervalMs);
            return;
        }
        if (sent->status < 200 || sent->status >= 300) {
            log::warn(kLog, "the ingest refused a crash report: " + std::to_string(sent->status));
            continue;  // stays listed; retried at the next flush
        }
        auto up = db_->prepare("UPDATE crashes SET sent_at = ?1, dump = NULL WHERE id = ?2");
        up.bind(1, now).bind(2, p.id);
        (void)up.run();
    }
}
std::vector<std::uint8_t> TelemetryService::crash_dump(const std::string& id) const {
    const std::lock_guard lock(mx_);
    auto st = db_->prepare("SELECT dump FROM crashes WHERE id = ?1 AND dump IS NOT NULL");
    st.bind(1, id);
    return st.step() ? st.col_blob(0) : std::vector<std::uint8_t>{};
}

json TelemetryService::view(int limit) const {
    const long long now = env_.clock->unix_millis();
    const auto p = env_.policy ? env_.policy() : nullptr;
    const json* crash = p ? p->find("telemetry.crash_reports") : nullptr;
    json out = {{"level", to_string(level())},
                {"source", level_source()},
                {"policy_version", p ? p->version() : 0},
                {"content_active", content_active()},
                {"crash_reports", crash && crash->is_boolean() ? crash->get<bool>() : true},
                {"pending", pending()},
                {"discarded", discarded()},
                {"records", json::array()}};
    const std::lock_guard lock(mx_);
    auto st = db_->prepare("SELECT id, body, sent_at FROM records WHERE at >= ?1 ORDER BY id DESC LIMIT ?2");
    st.bind(1, now - static_cast<long long>(kViewerDays) * 86'400'000).bind(2, limit);
    while (st.step()) {
        auto body = parse_json(st.col_text(1));
        if (!body) continue;
        json r = std::move(*body);
        r["id"] = st.col_int(0);
        r["state"] = st.col_null(2) ? "buffered" : "sent";
        if (!st.col_null(2)) r["sent_at"] = st.col_int(2);
        out["records"].push_back(std::move(r));
    }
    out["crashes"] = json::array();
    auto cs = db_->prepare("SELECT id, at, process, summary, size, sent_at, dump IS NOT NULL FROM crashes WHERE at >= ?1 ORDER BY at DESC");
    cs.bind(1, now - static_cast<long long>(kViewerDays) * 86'400'000);
    while (cs.step()) {
        auto s = parse_json(cs.col_text(3));
        json c = {{"id", cs.col_text(0)}, {"at", cs.col_int(1)}, {"process", cs.col_text(2)}, {"summary", s ? *s : json::object()},
                  {"size", cs.col_int(4)}, {"send_after", cs.col_int(1) + kCrashHoldMs}};
        c["state"] = !cs.col_null(5) ? "sent" : cs.col_int(6) ? (now < cs.col_int(1) + kCrashHoldMs ? "held" : "waiting") : "summary_only";
        out["crashes"].push_back(std::move(c));
    }
    return out;
}

}  // namespace wb::telemetry
