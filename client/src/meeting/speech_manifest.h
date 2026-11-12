#pragma once
// client/speech-models (add-meeting-capture D1, improve-skowork-and-knowledge-control D3): the portable part of the speech
// package. The archive layout, the manifest and the entry-name rules are checked here without any Windows API, so the
// fuzz job can exercise the parser. The Windows part (download, extraction, activation) is in speech_package.h.
#include "base/json.h"
#include "base/result.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace wb::meeting {

inline constexpr int kSpeechSchema = 1;
inline constexpr const char* kSpeechPackageName = "skowork-speech";
inline constexpr const char* kSpeechArchitecture = "windows-x64";
// Relative to the package root (the models\stt folder): every file the engine loads. A manifest that omits one is refused.
inline constexpr const char* kSpeechRequiredFiles[] = {
    "sherpa-onnx-c-api.dll",
    "onnxruntime.dll",
    "silero_vad.onnx",
    "parakeet-tdt-0.6b-v3-int8/encoder.int8.onnx",
    "parakeet-tdt-0.6b-v3-int8/decoder.int8.onnx",
    "parakeet-tdt-0.6b-v3-int8/joiner.int8.onnx",
    "parakeet-tdt-0.6b-v3-int8/tokens.txt",
    "whisper-small-int8/small-encoder.int8.onnx",
    "whisper-small-int8/small-decoder.int8.onnx",
    "whisper-small-int8/small-tokens.txt",
    "pyannote-segmentation-3-0/model.int8.onnx",
    "wespeaker_en_voxceleb_resnet34_LM.onnx",
};

struct SpeechManifestFile {
    std::string path;    // as in the archive, e.g. "models/stt/silero_vad.onnx"
    std::uint64_t bytes = 0;
    std::string sha256;  // lower-case hex
    std::string source;  // "models" or "runtime"
};

struct SpeechManifest {
    std::string version;  // folder name of the package: [A-Za-z0-9][A-Za-z0-9._-]{0,31}
    std::vector<SpeechManifestFile> files;
};

// Accepts exactly: "manifest.json", "licenses/<name>", and "models/stt/<relative path>" with segments of [A-Za-z0-9_.-].
// Rejects empty or overlong names, backslashes, drive letters, absolute and UNC forms, "." and ".." segments, and control
// characters. Directory entries are checked by the caller.
Status check_speech_entry_name(std::string_view name);

// Parses and validates manifest.json: schema 1, package skowork-speech, architecture windows-x64, a version, 1 to 64 files
// with unique valid paths under models/stt/, 64-hex digests, sizes between 1 and 2^40 bytes, sources "models" or
// "runtime", and every kSpeechRequiredFiles entry present.
Result<SpeechManifest> parse_speech_manifest(std::string_view text);

// Numeric-aware comparison of version folder names: negative, zero or positive like strcmp.
int compare_speech_versions(std::string_view a, std::string_view b);

// True for a version string that is safe as a folder name (the same rule parse_speech_manifest applies).
bool speech_version_ok(std::string_view version);

}  // namespace wb::meeting
