#pragma once
// The sherpa-onnx speech engine of the stt worker (add-meeting-capture design D1 and D4): Silero VAD per track, spoken
// language identification with Whisper small, Parakeet-TDT-0.6B-v3 for the languages it covers and Whisper small for
// the rest, and pyannote segmentation with WeSpeaker embeddings to split every track but the user's own into speakers.
// sherpa-onnx-c-api.dll and onnxruntime.dll are loaded at run time from the models package, so nothing else links
// them and a client without the package reports model_missing.
#include "meeting/core.h"

#include <filesystem>
#include <memory>

namespace wb::meeting {

// <exe dir>\models\stt. Development builds: WORKBENCH_STT_MODELS when set, else the repository's client/models/stt when
// the installed folder has no models (the DLLs still come from the installed folder).
std::filesystem::path stt_models_dir();
std::filesystem::path stt_runtime_dir();

// Fails with model_missing when the runtime or a model file is absent, and with "model_path" when a path has
// characters outside the ANSI code page (sherpa-onnx opens model files by narrow path). force_whisper sends every
// language to Whisper small (development builds use it to test the fallback, which no supported language reaches).
// The runtime and every model file are present (nothing is loaded). Development builds that use the test engine count
// as installed.
bool stt_installed();

Result<std::unique_ptr<SpeechEngine>> make_sherpa_engine(const std::filesystem::path& runtime_dir,
                                                         const std::filesystem::path& models_dir, int threads,
                                                         bool force_whisper = false);

}  // namespace wb::meeting
