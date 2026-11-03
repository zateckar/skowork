// Fuzz target for Skill packages (client/skills Skill Installation And Pinning; add-skills task 6.1): the client
// unpacks registry zips itself, so any bytes must give the files of a valid layout or an error, never a crash, an
// unsafe path or more than the limits.
#include "fuzz_main.h"
#include "skills/package.h"

#include <algorithm>

using namespace wb::skills;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::string_view bytes(reinterpret_cast<const char*>(data), size);
    PackageLimits limits;
    limits.max_entries = 50;
    limits.max_unpacked = 256 * 1024;
    const auto r = read_package(bytes, limits);
    if (r) {
        std::size_t total = 0;
        bool skill_md = false;
        for (const auto& [path, content] : *r) {
            if (!safe_relative_path(path)) __builtin_trap();
            total += content.size();
            if (path == "SKILL.md") skill_md = true;
        }
        if (!skill_md || total > limits.max_unpacked || r->size() > limits.max_entries) __builtin_trap();
    } else if (r.error().message.empty()) {
        __builtin_trap();
    }
    (void)safe_relative_path(bytes.substr(0, std::min<std::size_t>(size, 300)));
    return 0;
}
