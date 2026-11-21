// client/core-tools file tools: files.read, files.list, files.glob, files.grep, files.write and files.edit
// (add-core-tools-and-mcp design D2). Every path goes through the handle-checked path API of client/policy-enforcement;
// the tools never resolve paths themselves.
#pragma once

#include "policy/core.h"
#include "runtime/tools.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wb::tools {

// Document Text Extraction: the text of a document of `kind` (docx, xlsx, pptx, pdf) from its bytes, produced by the
// sandboxed extract worker.
using Extractor = std::function<Result<std::string>(const std::string& kind, const std::string& bytes, runtime::ToolContext& ctx)>;

// client/core-tools Seen File Content (protect-unread-overwrites design D1, D2): the state of a file as the agent last
// saw it in a Thread, after a full files.read or after a write of its own.
struct FileState {
    long long size = 0;
    long long modified = 0;  // last-write time, FILETIME as 100 ns ticks
    std::string sha256;      // lower-case hex of the bytes on disk
    friend bool operator==(const FileState&, const FileState&) = default;
};

// Per-Thread record of seen file states, keyed by canonical path. The client keeps it with the Thread (RunStore);
// tests and callers without a store use memory_seen_files().
class SeenFiles {
public:
    virtual ~SeenFiles() = default;
    virtual std::optional<FileState> get(const std::string& thread_id, const std::string& path) = 0;
    virtual void put(const std::string& thread_id, const std::string& path, const FileState& state) = 0;
};
std::shared_ptr<SeenFiles> memory_seen_files();

// The key a path is recorded under: its canonical form, lower-cased.
std::string seen_key(std::string_view path);

// Records the file's current state on disk as seen in the Thread, for files another tool wrote for the agent (the
// apps.create scaffold). Does nothing when the file cannot be read.
void record_seen_file(SeenFiles& seen, const std::string& thread_id, const std::string& path);

struct FileToolsEnv {
    std::function<std::shared_ptr<const policy::EffectivePolicy>()> policy;
    std::function<policy::PathRules()> rules;  // the filesystem rules in force (PolicyEnforcer::path_rules_now)
    std::string workspace;                     // relative paths are taken relative to it
    Extractor extract;                         // null: documents are refused like binary files
    std::shared_ptr<SeenFiles> seen;           // null: an in-memory record for these tools only
};

std::vector<runtime::ToolDef> file_tools(FileToolsEnv env);

// `path` with environment variables expanded, anchored at `workspace` when it is relative.
std::string absolute_path(std::string_view path, const std::string& workspace);

// Largest file the tools read into memory (text and documents).
inline constexpr long long kMaxFileBytes = 64ll << 20;

}  // namespace wb::tools
