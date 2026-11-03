// client/core-tools file tools: files.read, files.list, files.glob, files.grep, files.write and files.edit
// (add-core-tools-and-mcp design D2). Every path goes through the handle-checked path API of client/policy-enforcement;
// the tools never resolve paths themselves.
#pragma once

#include "policy/core.h"
#include "runtime/tools.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace wb::tools {

// Document Text Extraction: the text of a document of `kind` (docx, xlsx, pptx, pdf) from its bytes, produced by the
// sandboxed extract worker.
using Extractor = std::function<Result<std::string>(const std::string& kind, const std::string& bytes, runtime::ToolContext& ctx)>;

struct FileToolsEnv {
    std::function<std::shared_ptr<const policy::EffectivePolicy>()> policy;
    std::function<policy::PathRules()> rules;  // the filesystem rules in force (PolicyEnforcer::path_rules_now)
    std::string workspace;                     // relative paths are taken relative to it
    Extractor extract;                         // null: documents are refused like binary files
};

std::vector<runtime::ToolDef> file_tools(FileToolsEnv env);

// `path` with environment variables expanded, anchored at `workspace` when it is relative.
std::string absolute_path(std::string_view path, const std::string& workspace);

// Largest file the tools read into memory (text and documents).
inline constexpr long long kMaxFileBytes = 64ll << 20;

}  // namespace wb::tools
