// A Skill package from the registry (add-skills design D6 Package): a zip with SKILL.md at its root or inside one
// top-level folder, plus README and LICENSE files and the optional folders scripts/, references/ and assets/. The
// registry checks the same rules on upload; the client checks them again before it writes anything.
#pragma once

#include "base/result.h"

#include <cstddef>
#include <map>
#include <string>
#include <string_view>

namespace wb::skills {

struct PackageLimits {
    std::size_t max_bytes = 5u * 1024 * 1024;       // the zip
    std::size_t max_entries = 200;
    std::size_t max_unpacked = 20u * 1024 * 1024;   // all files together
};

// The files of the package by relative path ("SKILL.md", "scripts/run.ps1"), with a single top-level folder removed.
// Fails on anything else: encrypted or unsafe entries, duplicates, other top-level entries, sizes over the limits.
Result<std::map<std::string, std::string>> read_package(std::string_view zip, const PackageLimits& limits = {});

// Whether a relative path is safe to create under a folder: forward slashes, no empty, "." or ".." segments, no drive,
// no control or reserved characters, no Windows device names.
[[nodiscard]] bool safe_relative_path(std::string_view path);

}  // namespace wb::skills
