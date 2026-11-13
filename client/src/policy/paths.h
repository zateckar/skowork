// Path canonicalisation and handle-checked file access (client/policy-enforcement Filesystem Path Rules; design D4).
// Windows only.
#pragma once

#include "base/result.h"
#include "platform/win.h"
#include "policy/core.h"

#include <string>
#include <string_view>

namespace wb::policy {

// Expands %VARIABLES% (ExpandEnvironmentStringsW). `extra` adds variables the process environment lacks, such as
// SKOWORK_DATA.
std::string expand_environment(std::string_view s, const std::vector<std::pair<std::string, std::string>>& extra = {});

// The final path of `path`: environment variables expanded, relative segments resolved (GetFullPathNameW), device
// namespaces and alternate data streams refused, then junctions, symbolic links and 8.3 names resolved through
// GetFinalPathNameByHandleW on the deepest existing ancestor. Missing trailing components are appended as given.
Result<std::string> canonicalize(std::string_view path);

// The final path of an open handle, normalised (no \\?\ prefix).
Result<std::string> final_path_of(HANDLE h);

// Opens an existing file or folder for reading and checks the path of the opened handle, so that a link swapped in
// after the check cannot redirect the read. Error code "policy_denied" carries the rule in the message.
Result<unique_file> open_for_read(std::string_view path, const PathRules& rules, RuleVerdict* verdict = nullptr);

enum class WriteMode { create_new, open_or_create, truncate_or_create };

// Opens a file for writing without a time-of-check-to-time-of-use gap: the parent folder is opened first, its final
// path is checked, and the file is created relative to that open folder handle (NtCreateFile with a RootDirectory).
// The opened file is checked again by handle; a file with more than one hard link is refused.
Result<unique_file> open_for_write(std::string_view path, WriteMode mode, const PathRules& rules, RuleVerdict* verdict = nullptr);

// Writes a whole file atomically, with the same checks as open_for_write: the content goes to a temporary file created
// in the opened, checked parent folder, which is then renamed over the target relative to that folder handle. A link
// or a file with other hard links in the target's place is refused. Readers see the old or the new content, never a
// mix. Returns the final path written.
Result<std::string> write_file_atomic(std::string_view path, std::string_view content, const PathRules& rules, RuleVerdict* verdict = nullptr);

// A folder a rule names, as its final path (canonicalize), or normalised as given when it cannot be resolved.
std::string final_folder(const std::string& folder);
std::vector<std::string> final_folders(std::vector<std::string> folders);

// A path glob with its literal leading folders as a final path and its wildcard part as written. A glob that starts
// with a wildcard is returned as given.
std::string final_glob(const std::string& glob);

// The filesystem rules of an effective policy with environment variables expanded; %SKOWORK_DATA% is the data root.
// The folders the rules name (globs, workspace, data root, grants) are final paths, like the paths compared with them.
PathRules path_rules(const EffectivePolicy* policy, const std::string& workspace, const std::string& data_root,
                     std::vector<FolderGrant> grants);

}  // namespace wb::policy
