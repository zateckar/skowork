// Fuzz target for the speech package manifest and archive entry names (client/speech-models, tasks 5.2): the manifest
// arrives in a downloaded or imported archive, so any bytes must give a valid manifest or an error, and every accepted
// entry name must stay inside the package layout.
#include "fuzz_main.h"
#include "meeting/speech_manifest.h"

#include <string>

using namespace wb::meeting;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::string_view bytes(reinterpret_cast<const char*>(data), size);
    const auto m = parse_speech_manifest(bytes);
    if (m) {
        if (!speech_version_ok(m->version) || m->files.empty() || m->files.size() > 64) __builtin_trap();
        for (const auto& f : m->files) {
            if (f.path.rfind("models/stt/", 0) != 0 || !check_speech_entry_name(f.path).ok() || f.bytes == 0 || f.sha256.size() != 64)
                __builtin_trap();
        }
    } else if (m.error().message.empty()) {
        __builtin_trap();
    }
    const auto name = check_speech_entry_name(bytes);
    if (name.ok() && (bytes.find("..") != std::string_view::npos || bytes.find('\\') != std::string_view::npos)) __builtin_trap();
    return 0;
}
