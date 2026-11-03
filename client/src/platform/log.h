// Rotating diagnostic logs (client/desktop-host Diagnostic Logs): at most 50 MB in total, oldest files deleted
// first, every line passed through secret redaction. Verbose diagnostics can be enabled for a limited time.
#pragma once

#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>

namespace wb::log {

enum class Level { debug, info, warn, error };

struct Options {
    std::filesystem::path dir;
    size_t max_total_bytes = 50u * 1024 * 1024;
    size_t max_file_bytes = 5u * 1024 * 1024;
    bool also_stderr = false;
};

void init(const Options& options);
void shutdown();

// Verbose mode logs debug lines; it switches itself off at `until`.
void set_verbose_until(std::chrono::system_clock::time_point until);
bool verbose();
// End of verbose mode in unix seconds, or 0 when it is off.
long long verbose_until_unix();

void write(Level level, std::string_view component, std::string_view message);

inline void debug(std::string_view c, std::string_view m) { write(Level::debug, c, m); }
inline void info(std::string_view c, std::string_view m) { write(Level::info, c, m); }
inline void warn(std::string_view c, std::string_view m) { write(Level::warn, c, m); }
inline void error(std::string_view c, std::string_view m) { write(Level::error, c, m); }

// Deletes the oldest files in `dir` until the total size of *.log files is at most `max_total` (exposed for tests).
void enforce_cap(const std::filesystem::path& dir, size_t max_total);

}  // namespace wb::log
