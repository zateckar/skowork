#pragma once
// Third-party notices (client/desktop-host Third-Party Notices): every component and model the client ships, as listed
// in client/third_party/README.md. Shown in About through app.version. Keep in step with the manifest.
#include <nlohmann/json.hpp>

namespace wb {

inline nlohmann::json third_party_notices() {
    using nlohmann::json;
    auto entry = [](const char* name, const char* version, const char* licence, const char* package,
                    const char* attribution = "") {
        json e{{"name", name}, {"version", version}, {"licence", licence}, {"package", package}};
        if (*attribution) e["attribution"] = attribution;
        return e;
    };
    return json::array({
        entry("SQLite3 Multiple Ciphers", "2.6.1 (SQLite 3.54.0)", "MIT; SQLite: public domain", "core"),
        entry("JSON for Modern C++", "3.12.0", "MIT", "core"),
        entry("miniz", "3.1.2", "MIT", "core"),
        entry("pugixml", "1.16", "MIT", "core"),
        entry("PDFium", "chromium/8111", "BSD-3-Clause, with the licences of its bundled libraries", "core"),
        entry("MD4C", "0.5.2", "MIT", "core"),
        entry("sherpa-onnx", "2.0.1", "Apache-2.0", "speech-to-text"),
        entry("ONNX Runtime", "as bundled by sherpa-onnx 2.0.1", "MIT", "speech-to-text"),
        entry("Parakeet-TDT-0.6B-v3", "int8", "CC-BY-4.0", "speech-to-text",
              "Parakeet-TDT-0.6B-v3 by NVIDIA, licensed under CC BY 4.0 "
              "(https://creativecommons.org/licenses/by/4.0/). Converted to ONNX and quantized to int8 by the "
              "sherpa-onnx project; not modified otherwise."),
        entry("Whisper small", "int8", "MIT", "speech-to-text", "Whisper by OpenAI."),
        entry("Silero VAD", "", "MIT", "speech-to-text", "Silero VAD by Silero Team."),
        entry("pyannote segmentation 3.0", "int8", "MIT", "speech-to-text", "pyannote.audio by Hervé Bredin."),
        entry("WeSpeaker ResNet34-LM", "VoxCeleb", "Apache-2.0", "speech-to-text", "WeSpeaker by the WeNet community."),
    });
}

}  // namespace wb
