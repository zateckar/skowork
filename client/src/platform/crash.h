// Crash capture (client/telemetry Crash Reports; add-telemetry-and-admin design D4): an unhandled-exception filter
// that writes a filtered minidump from a writer thread started in advance, the inherited dump file a worker writes
// into, and the summary the Telemetry viewer shows.
#pragma once

#include "base/result.h"
#include "platform/win.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wb::crash {

// Thread stacks and the module list, with memory filtered down to what a stack walk needs and module paths removed.
unsigned long dump_type();

// The core: on an unhandled exception (or std::terminate), writes <dir>/<process>-<unix ms>.dmp. Call once, early.
void install(const std::filesystem::path& dir, std::string_view process);
// A worker: writes the dump into the file the core passed down (`--crash-dump <handle>`).
void install_to_handle(HANDLE file);
// Whether a crash is captured at all (policy.telemetry.crash_reports, telemetry level not off). Default on.
void set_enabled(bool on);

// A dump file the core creates for a worker it starts: the handle is inheritable and goes on the command line.
struct DumpSlot {
    unique_file file;
    std::filesystem::path path;
};
Result<DumpSlot> open_dump_slot(const std::filesystem::path& dir, std::string_view process);
std::wstring dump_argument(const DumpSlot& slot);  // L"--crash-dump <handle>"
// After the process ended: the dump when the worker wrote one. An empty file is deleted.
std::optional<std::filesystem::path> take_dump(DumpSlot& slot);
// The handle value from `--crash-dump`; nullptr when it does not parse.
HANDLE parse_dump_argument(std::wstring_view value);

struct Summary {
    std::string exception_code;  // "0xC000001D"
    std::string module;          // the module the faulting address lies in ("" when none)
    int threads = 0;
    std::vector<std::string> modules;  // file names only
};
Result<Summary> summarize(const std::filesystem::path& dump);

}  // namespace wb::crash
