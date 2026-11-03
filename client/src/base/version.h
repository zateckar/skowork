#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace wb {

struct Version {
    int major = 0, minor = 0, patch = 0;
    std::string pre;  // pre-release tag without '-', empty for a release
};

std::optional<Version> parse_version(std::string_view s);  // "1.2.3", "v1.2.3", "1.2.3-beta.1", "1.2"
int compare_versions(const Version& a, const Version& b);   // <0, 0, >0 (semver precedence, build ignored)

// True when `current` is below `minimum`. An unparsable minimum never blocks.
bool below_minimum(std::string_view current, std::string_view minimum);

}  // namespace wb
