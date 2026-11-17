// Fuzz target for the speech package archive reader (client/speech-models, task 5.2): any bytes, read as a zip, must
// give a plan or an error. An accepted plan has only safe, unique names within the entry, size and total limits, a
// manifest that lists exactly the model files, and every entry streams exactly its declared size (never more).
#include "fuzz_main.h"
#include "base/strings.h"
#include "meeting/speech_archive.h"

#include <cstring>
#include <set>
#include <string>

using namespace wb::meeting;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    SpeechArchive a;
    const auto read = [data, size](std::uint64_t ofs, void* buf, size_t n) -> size_t {
        if (ofs >= size) return 0;
        n = std::min<size_t>(n, size - static_cast<size_t>(ofs));
        std::memcpy(buf, data + ofs, n);
        return n;
    };
    if (!a.open(read, size).ok()) return 0;
    SpeechLimits lim;
    lim.max_entry = 1u << 20;
    lim.max_total = 4u << 20;
    lim.max_entries = 64;
    const auto plan = a.inspect(lim);
    if (!plan) {
        if (plan.error().message.empty()) __builtin_trap();
        return 0;
    }
    if (plan->entries.empty() || plan->entries.size() > lim.max_entries || plan->total > lim.max_total) __builtin_trap();
    std::set<std::string> names;
    size_t listed = 0;
    bool manifest = false;
    std::uint64_t total = 0;
    for (const auto& e : plan->entries) {
        if (!check_speech_entry_name(e.name).ok() || e.size > lim.max_entry) __builtin_trap();
        if (!names.insert(wb::to_lower(e.name)).second) __builtin_trap();
        if (e.name == "manifest.json") manifest = true;
        if (e.listed >= 0) {
            ++listed;
            const auto& f = plan->manifest.files.at(static_cast<size_t>(e.listed));
            if (f.path != e.name || f.bytes != e.size) __builtin_trap();
        } else if (e.name != "manifest.json" && e.name.rfind("licenses/", 0) != 0) {
            __builtin_trap();
        }
        total += e.size;
        std::uint64_t got = 0;
        const auto s = a.extract(e, [&](const char*, size_t n) { got += n; return got <= e.size; });
        if (got > e.size || (s.ok() && got != e.size)) __builtin_trap();
    }
    if (!manifest || listed != plan->manifest.files.size() || total != plan->total) __builtin_trap();
    return 0;
}
