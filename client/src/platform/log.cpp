#include "platform/log.h"

#include "base/redact.h"
#include "base/time.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <vector>

namespace wb::log {
namespace {

struct State {
    std::mutex mx;
    Options opt;
    std::ofstream out;
    std::filesystem::path current;
    size_t current_size = 0;
    bool ready = false;
    std::atomic<long long> verbose_until_ms{0};
};

State& st() {
    static State s;
    return s;
}

void open_new_file(State& s) {
    s.out.close();
    // Names sort chronologically: creation time, then a sequence number for files opened in the same millisecond.
    static unsigned seq = 0;
    char name[64];
    std::snprintf(name, sizeof name, "skowork-%013lld-%04u.log", unix_millis_now(), seq++ % 10000);
    s.current = s.opt.dir / name;
    s.out.open(s.current, std::ios::app | std::ios::binary);
    s.current_size = 0;
    enforce_cap(s.opt.dir, s.opt.max_total_bytes);
}

const char* level_name(Level l) {
    switch (l) {
    case Level::debug: return "DEBUG";
    case Level::info: return "INFO ";
    case Level::warn: return "WARN ";
    case Level::error: return "ERROR";
    }
    return "?";
}

}  // namespace

void init(const Options& options) {
    auto& s = st();
    const std::lock_guard lock(s.mx);
    s.opt = options;
    std::error_code ec;
    std::filesystem::create_directories(s.opt.dir, ec);
    open_new_file(s);
    s.ready = true;
}

void shutdown() {
    auto& s = st();
    const std::lock_guard lock(s.mx);
    s.out.close();
    s.ready = false;
}

void set_verbose_until(std::chrono::system_clock::time_point until) {
    st().verbose_until_ms = std::chrono::duration_cast<std::chrono::milliseconds>(until.time_since_epoch()).count();
}

bool verbose() { return unix_millis_now() < st().verbose_until_ms.load(); }

long long verbose_until_unix() { return verbose() ? st().verbose_until_ms.load() / 1000 : 0; }

void write(Level level, std::string_view component, std::string_view message) {
    if (level == Level::debug && !verbose()) return;
    // Redaction happens before anything touches the disk, in every mode.
    std::string line = iso8601_utc_now() + " " + level_name(level) + " [" + std::string(component) + "] " +
                       redact_secrets(message) + "\n";
    auto& s = st();
    const std::lock_guard lock(s.mx);
    if (s.opt.also_stderr) std::fputs(line.c_str(), stderr);
    if (!s.ready) return;
    if (s.current_size + line.size() > s.opt.max_file_bytes) open_new_file(s);
    s.out.write(line.data(), static_cast<std::streamsize>(line.size()));
    s.out.flush();
    s.current_size += line.size();
}

void enforce_cap(const std::filesystem::path& dir, size_t max_total) {
    std::error_code ec;
    std::vector<std::pair<std::filesystem::path, std::filesystem::path>> files;  // (file name, path)
    size_t total = 0;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        if (!e.is_regular_file(ec) || e.path().extension() != ".log") continue;
        total += static_cast<size_t>(e.file_size(ec));
        files.emplace_back(e.path().filename(), e.path());
    }
    std::sort(files.begin(), files.end());
    for (const auto& [t, p] : files) {
        if (total <= max_total) break;
        if (p == st().current) continue;  // never delete the file being written
        const auto size = static_cast<size_t>(std::filesystem::file_size(p, ec));
        if (std::filesystem::remove(p, ec)) total -= size;
    }
}

}  // namespace wb::log
