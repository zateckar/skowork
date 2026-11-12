#pragma once
// The sherpa-onnx speech engine of the stt worker (add-meeting-capture design D1 and D4): Silero VAD per track, spoken
// language identification with Whisper small, Parakeet-TDT-0.6B-v3 for the languages it covers and Whisper small for
// the rest, and pyannote segmentation with WeSpeaker embeddings to split every track but the user's own into speakers.
// sherpa-onnx-c-api.dll and onnxruntime.dll are loaded at run time from the speech package (client/speech-models), so
// nothing else links them. The package folder is <data root>\speech\packages\<version>\models\stt; the desktop host
// passes it to the worker with each request. No other location is read (task 5.6).
#include "meeting/core.h"

#include <filesystem>
#include <memory>

namespace wb::meeting {

// True when the runtime and every required file are present in `package_dir` (nothing is loaded). An empty path is not
// installed. Development builds that use the test engine count as installed.
bool stt_installed(const std::filesystem::path& package_dir);

// Fails with model_missing when the runtime or a model file is absent, and with "model_path" when a path cannot be
// represented as UTF-8. force_whisper sends every language to Whisper small (development builds use it to test the
// fallback, which no supported language reaches).
Result<std::unique_ptr<SpeechEngine>> make_sherpa_engine(const std::filesystem::path& runtime_dir,
                                                         const std::filesystem::path& models_dir, int threads,
                                                         bool force_whisper = false);

}  // namespace wb::meeting
