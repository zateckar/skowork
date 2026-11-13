// The core side of Document Text Extraction (add-core-tools-and-mcp design D4): every document is extracted by a fresh
// `skowork-worker.exe --role extract` process under client/tool-sandbox (restricted token, no network, the extraction
// worker's memory budget from client/desktop-host, one process, 30 seconds). A crash, hang or runaway parser ends that
// process only; the tool gets an extraction error.
#pragma once

#include "sandbox/sandbox.h"
#include "tools/files.h"

#include <chrono>
#include <filesystem>
#include <functional>
#include <optional>

namespace wb::tools {

struct SandboxExtractorOptions {
    std::filesystem::path worker_exe;  // skowork-worker.exe
    sandbox::Sandbox* sandbox = nullptr;
    std::chrono::milliseconds timeout{30000};
    long long memory_mb = 1024;  // client/desktop-host Worker Memory Budgets: the extraction worker
    // Crash Reports (client/telemetry): the folder for dumps while crash reports are enabled, and the dump's destination.
    std::function<std::optional<std::filesystem::path>()> crash_dir;
    std::function<void(const std::filesystem::path& dump, const std::string& process)> on_crash;
};

Extractor sandbox_extractor(SandboxExtractorOptions options);

}  // namespace wb::tools
