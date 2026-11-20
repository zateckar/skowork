#include "meeting/sherpa_engine.h"

#include "base/strings.h"
#include "platform/win.h"

#include <sherpa-onnx/c-api/c-api.h>

#include <psapi.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <type_traits>

namespace wb::meeting {
namespace {

namespace fs = std::filesystem;

// The C API functions the engine calls, resolved from the DLL by name.
#define WB_SHERPA_FUNCTIONS(X)                                \
    X(SherpaOnnxGetVersionStr)                                 \
    X(SherpaOnnxCreateOfflineRecognizer)                       \
    X(SherpaOnnxDestroyOfflineRecognizer)                      \
    X(SherpaOnnxCreateOfflineStream)                           \
    X(SherpaOnnxDestroyOfflineStream)                          \
    X(SherpaOnnxAcceptWaveformOffline)                         \
    X(SherpaOnnxDecodeMultipleOfflineStreams)                  \
    X(SherpaOnnxGetOfflineStreamResult)                        \
    X(SherpaOnnxDestroyOfflineRecognizerResult)                \
    X(SherpaOnnxCreateVoiceActivityDetector)                   \
    X(SherpaOnnxDestroyVoiceActivityDetector)                  \
    X(SherpaOnnxVoiceActivityDetectorAcceptWaveform)           \
    X(SherpaOnnxVoiceActivityDetectorEmpty)                    \
    X(SherpaOnnxVoiceActivityDetectorFront)                    \
    X(SherpaOnnxVoiceActivityDetectorPop)                      \
    X(SherpaOnnxVoiceActivityDetectorFlush)                    \
    X(SherpaOnnxDestroySpeechSegment)                          \
    X(SherpaOnnxCreateSpokenLanguageIdentification)            \
    X(SherpaOnnxDestroySpokenLanguageIdentification)           \
    X(SherpaOnnxSpokenLanguageIdentificationCreateOfflineStream) \
    X(SherpaOnnxSpokenLanguageIdentificationCompute)           \
    X(SherpaOnnxDestroySpokenLanguageIdentificationResult)     \
    X(SherpaOnnxCreateOfflineSpeakerDiarization)               \
    X(SherpaOnnxDestroyOfflineSpeakerDiarization)              \
    X(SherpaOnnxOfflineSpeakerDiarizationProcess)              \
    X(SherpaOnnxOfflineSpeakerDiarizationResultGetNumSegments) \
    X(SherpaOnnxOfflineSpeakerDiarizationResultSortByStartTime) \
    X(SherpaOnnxOfflineSpeakerDiarizationDestroySegment)       \
    X(SherpaOnnxOfflineSpeakerDiarizationDestroyResult)

void trace(const char* stage);

struct Api {
#define WB_DECLARE(f) std::add_pointer_t<decltype(::f)> f = nullptr;
    WB_SHERPA_FUNCTIONS(WB_DECLARE)
#undef WB_DECLARE
};

// Loaded once per worker process and kept for its lifetime.
Result<const Api*> load_api(const fs::path& runtime_dir) {
    static std::mutex mx;
    static std::unique_ptr<Api> loaded;
    const std::lock_guard lock(mx);
    if (loaded) return loaded.get();
    const fs::path dll = runtime_dir / L"sherpa-onnx-c-api.dll";
    std::error_code ec;
    if (!fs::exists(dll, ec)) return make_error("model_missing", "The speech-to-text runtime is not installed.");
    // An absolute path with the altered search order finds onnxruntime.dll next to it.
    HMODULE m = ::LoadLibraryExW(dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!m) return make_error("model_missing", "The speech-to-text runtime does not load: " + error_text(::GetLastError()));
    auto api = std::make_unique<Api>();
#define WB_RESOLVE(f)                                                                                       \
    api->f = reinterpret_cast<decltype(api->f)>(reinterpret_cast<void (*)()>(::GetProcAddress(m, #f))); \
    if (!api->f) return make_error("model_missing", "The speech-to-text runtime has no " #f ".");
    WB_SHERPA_FUNCTIONS(WB_RESOLVE)
#undef WB_RESOLVE
    loaded = std::move(api);
    return loaded.get();
}

// The worker and desktop host opt into UTF-8 as the Windows active code page. The sherpa C API takes narrow paths;
// passing UTF-8 preserves names that cannot be represented by the user's legacy ANSI code page.
Result<std::string> model_path(const fs::path& p) {
    const std::wstring w = p.wstring();
    std::string s = narrow(w);
    if (s.empty() || widen(s) != w) return make_error("model_path", "The speech-to-text model path is invalid.");
    return s;
}

struct Paths {
    std::string vad, parakeet_encoder, parakeet_decoder, parakeet_joiner, parakeet_tokens, whisper_encoder, whisper_decoder,
        whisper_tokens, segmentation, embedding;
};

Result<Paths> model_paths(const fs::path& dir) {
    Paths p;
    const std::pair<std::string*, const wchar_t*> files[] = {
        {&p.vad, L"silero_vad.onnx"},
        {&p.parakeet_encoder, L"parakeet-tdt-0.6b-v3-int8/encoder.int8.onnx"},
        {&p.parakeet_decoder, L"parakeet-tdt-0.6b-v3-int8/decoder.int8.onnx"},
        {&p.parakeet_joiner, L"parakeet-tdt-0.6b-v3-int8/joiner.int8.onnx"},
        {&p.parakeet_tokens, L"parakeet-tdt-0.6b-v3-int8/tokens.txt"},
        {&p.whisper_encoder, L"whisper-small-int8/small-encoder.int8.onnx"},
        {&p.whisper_decoder, L"whisper-small-int8/small-decoder.int8.onnx"},
        {&p.whisper_tokens, L"whisper-small-int8/small-tokens.txt"},
        {&p.segmentation, L"pyannote-segmentation-3-0/model.int8.onnx"},
        {&p.embedding, L"wespeaker_en_voxceleb_resnet34_LM.onnx"},
    };
    for (const auto& [out, rel] : files) {
        const fs::path f = (dir / rel).lexically_normal();
        std::error_code ec;
        if (!fs::is_regular_file(f, ec)) return make_error("model_missing", "The speech-to-text models are not installed.");
        auto a = model_path(f);
        if (!a) return a.error();
        *out = std::move(*a);
    }
    return p;
}

}  // namespace

bool stt_installed(const fs::path& package_dir) {
#ifdef SKOWORK_DEV_ENDPOINTS
    wchar_t v[32] = {};
    if (::GetEnvironmentVariableW(L"SKOWORK_STT_ENGINE", v, 32) == 0 || std::wstring(v) == L"test") return true;
#endif
    if (package_dir.empty()) return false;
    std::error_code ec;
    return fs::exists(package_dir / L"sherpa-onnx-c-api.dll", ec) && static_cast<bool>(model_paths(package_dir));
}

namespace {

std::vector<float> to_float(const std::int16_t* pcm, size_t n) {
    std::vector<float> f(n);
    for (size_t i = 0; i < n; ++i) f[i] = static_cast<float>(pcm[i]) / 32768.0f;
    return f;
}

constexpr std::int32_t kRate = 16000;
constexpr size_t kBatch = 8;               // segments decoded together
// and at most this much audio. onnxruntime keeps its arena at the largest batch it has seen, and Parakeet's activations
// grow with the batch's audio: a 40 s batch took the worker from 943 MB to 1,483 MB and a later one past its 2 GB.
constexpr size_t kBatchSamples = 20 * kRate;
constexpr size_t kLanguageWindow = 30 * kRate;  // Whisper's input length
constexpr size_t kLanguageMinimum = 8 * kRate;  // speech per language vote
constexpr int kLanguageVotes = 3;

// The hotword list and the BPE vocabulary sherpa-onnx needs to encode it, written into the worker's temporary folder
// for the moment the recognizer is created, then removed.
struct HotwordFiles {
    fs::path hotwords, vocab;

    bool write(const std::string& tokens_file, const std::vector<std::string>& terms) {
        wchar_t tmp[MAX_PATH + 1] = {};
        if (::GetTempPathW(MAX_PATH + 1, tmp) == 0) return false;
        const std::wstring stem = L"skowork-stt-" + std::to_wstring(::GetCurrentProcessId()) + L"-" + std::to_wstring(::GetTickCount64());
        hotwords = fs::path(tmp) / (stem + L".hotwords");
        vocab = fs::path(tmp) / (stem + L".bpe.vocab");
        std::ifstream in(fs::path(widen(tokens_file)), std::ios::binary);
        std::ofstream v(vocab, std::ios::binary | std::ios::trunc);
        if (!in || !v) return remove(), false;
        // tokens.txt: "<token> <id>" per line. The vocabulary takes "<token>\t-<id>", without the special <...> tokens.
        for (std::string line; std::getline(in, line);) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const auto sp = line.rfind(' ');
            if (sp == std::string::npos || sp == 0) continue;
            const std::string token = line.substr(0, sp), id = line.substr(sp + 1);
            if (token.size() > 2 && token.front() == '<' && token.back() == '>') continue;
            v << token << '\t' << '-' << id << '\n';
        }
        std::ofstream h(hotwords, std::ios::binary | std::ios::trunc);
        size_t n = 0;
        for (const auto& t : terms) {
            std::string clean;
            for (const char ch : t) clean.push_back(ch == ':' || ch == '\n' || ch == '\r' || ch == '\t' ? ' ' : ch);
            clean = trim(clean);
            if (clean.empty()) continue;
            h << clean << '\n';
            ++n;
        }
        v.close();
        h.close();
        if (!v || !h || n == 0) return remove(), false;
        return true;
    }
    void remove() {
        std::error_code ec;
        if (!hotwords.empty()) fs::remove(hotwords, ec);
        if (!vocab.empty()) fs::remove(vocab, ec);
    }
};

class SherpaEngine : public SpeechEngine {
public:
    SherpaEngine(const Api* api, Paths paths, int threads, bool force_whisper)
        : api_(api), paths_(std::move(paths)), threads_(threads), force_whisper_(force_whisper) {}

    Result<json> transcribe(const std::vector<TrackAudio>& tracks, const TranscribeOptions& options) override {
        for (const auto& t : tracks)
            if (static_cast<std::int32_t>(t.rate) != kRate) return make_error("invalid", "tracks are 16 kHz");
        // 1. Speech spans per track: VAD runs over the whole track, so spans stay the same from pass to pass; only those
        // that start at or after from_ms are decoded (show-meeting-transcription-progress D1).
        std::vector<std::vector<Span>> all, spans;
        for (const auto& t : tracks) {
            auto s = vad(t);
            if (!s) return s.error();
            std::vector<Span> todo;
            for (const auto& x : *s)
                if (to_ms(x.begin) >= options.from_ms) todo.push_back(x);
            all.push_back(std::move(*s));
            spans.push_back(std::move(todo));
        }
        trace("vad done");
        // 2. Speakers: the user's own track is "me"; every other track is diarized whole, its speakers numbered in one
        // sequence across tracks. Diarization runs before ASR so the two models are never loaded together. A live pass
        // skips it: its speakers are provisional, and the turns of the final diarization label every kept segment.
        std::vector<std::vector<std::string>> speakers(tracks.size());
        json turns_out = json::object();
        int next = 1;
        for (size_t i = 0; i < tracks.size(); ++i) {
            if (tracks[i].track == "local") {
                speakers[i].assign(spans[i].size(), "me");
                continue;
            }
            std::vector<Turn> turns;
            if (options.diarize && all[i].size() > 1) {
                trace("diarization start");
                auto d = diarize(tracks[i]);
                trace("diarization done");
                if (!d) return d.error();
                turns = std::move(*d);
            }
            if (options.diarize) {
                json a = json::array();
                for (const auto& t : turns) a.push_back({t.start_ms, t.end_ms, t.cluster});
                turns_out[tracks[i].track] = a;
            }
            std::vector<std::pair<long long, long long>> ms;
            for (const auto& s : spans[i]) ms.emplace_back(to_ms(s.begin), to_ms(s.end));
            speakers[i] = options.diarize ? label_spans(ms, turns, next) : std::vector<std::string>(spans[i].size(), "s1");
        }
        // 3. Language: the requested one, else a vote over up to three windows of speech, the user's own first.
        std::string lang = to_lower(options.language);
        if (lang.empty()) {
            auto l = identify(tracks, spans);
            if (!l) return l.error();
            lang = *l;
        }
        json out = {{"language", lang}, {"segments", json::array()}, {"engine", std::string("sherpa-onnx ") + api_->SherpaOnnxGetVersionStr()}};
        if (options.diarize) out["turns"] = turns_out;
        if (!supported_language(lang)) return out;  // the worker reports unsupported_language
        // 4. ASR in decoding units (Speech Decoded In Context).
        const bool parakeet = !force_whisper_ && parakeet_language(lang);
        out["asr"] = parakeet ? "parakeet-tdt-0.6b-v3" : "whisper-small";
        auto texts = recognize(tracks, spans, parakeet, lang);
        if (!texts) return texts.error();
        // 5. Segments in another script, or short fragments in another language, are decoded again by Whisper small with
        // the meeting's language set; what still does not fit is unclear (Transcript Keeps To The Meeting Language).
        std::vector<std::vector<bool>> unclear(tracks.size());
        for (size_t i = 0; i < tracks.size(); ++i) unclear[i].assign(spans[i].size(), false);
        if (parakeet) {
            auto again = redecode(tracks, spans, *texts, lang, unclear);
            if (!again) return again.error();
        }
        for (size_t i = 0; i < tracks.size(); ++i)
            for (size_t k = 0; k < spans[i].size(); ++k) {
                const std::string& text = (*texts)[i][k];
                if (text.empty() && !unclear[i][k]) continue;
                json seg = {{"track", tracks[i].track},
                            {"start_ms", to_ms(spans[i][k].begin)},
                            {"end_ms", to_ms(spans[i][k].end)},
                            {"speaker", speakers[i][k]},
                            {"text", unclear[i][k] ? std::string("(unclear)") : text}};
                if (unclear[i][k]) seg["unclear"] = true;
                out["segments"].push_back(std::move(seg));
            }
        return out;
    }

private:
    static long long to_ms(size_t sample) { return static_cast<long long>(sample) * 1000 / kRate; }

    Result<std::vector<Span>> vad(const TrackAudio& t) const {
        SherpaOnnxVadModelConfig c{};
        c.silero_vad.model = paths_.vad.c_str();
        c.silero_vad.threshold = 0.5f;
        c.silero_vad.min_silence_duration = 0.4f;
        c.silero_vad.min_speech_duration = 0.25f;
        c.silero_vad.window_size = 512;
        c.silero_vad.max_speech_duration = 20.0f;  // Whisper takes at most 30 s; shorter segments also time the transcript
        c.sample_rate = kRate;
        c.num_threads = 1;
        c.provider = "cpu";
        const auto* v = api_->SherpaOnnxCreateVoiceActivityDetector(&c, 60.0f);
        if (!v) return make_error("stt_failed", "The voice activity detector does not start.");
        std::vector<Span> out;
        const auto drain = [&] {
            while (!api_->SherpaOnnxVoiceActivityDetectorEmpty(v)) {
                const auto* s = api_->SherpaOnnxVoiceActivityDetectorFront(v);
                if (s && s->start >= 0 && s->n > 0)
                    out.push_back({static_cast<size_t>(s->start), static_cast<size_t>(s->start) + static_cast<size_t>(s->n), 0});
                api_->SherpaOnnxDestroySpeechSegment(s);
                api_->SherpaOnnxVoiceActivityDetectorPop(v);
            }
        };
        std::vector<float> win;
        for (size_t i = 0; i < t.pcm.size(); i += 512) {
            const size_t k = std::min<size_t>(512, t.pcm.size() - i);
            win = to_float(t.pcm.data() + i, k);
            api_->SherpaOnnxVoiceActivityDetectorAcceptWaveform(v, win.data(), static_cast<std::int32_t>(k));
            drain();
        }
        api_->SherpaOnnxVoiceActivityDetectorFlush(v);
        drain();
        api_->SherpaOnnxDestroyVoiceActivityDetector(v);
        for (auto& s : out) s.end = std::min(s.end, t.pcm.size());
        return out;
    }

    Result<std::vector<Turn>> diarize(const TrackAudio& t) const {
        SherpaOnnxOfflineSpeakerDiarizationConfig c{};
        c.segmentation.pyannote.model = paths_.segmentation.c_str();
        // A 5 s step over the 10 s windows (half overlap) instead of the default 1 s: clustering holds a distance matrix over
        // every window and local speaker, which at 1 s outgrows the worker's 2 GB on an hour-long track (about 10,800
        // embeddings); at 5 s an hour is about 2,200.
        c.segmentation.pyannote.window_shift_ratio = 0.5f;
        c.segmentation.num_threads = threads_;
        c.segmentation.provider = "cpu";
        c.embedding.model = paths_.embedding.c_str();
        c.embedding.num_threads = threads_;
        c.embedding.provider = "cpu";
        c.clustering.num_clusters = -1;  // unknown: a distance threshold decides
        c.clustering.threshold = 0.5f;
        c.min_duration_on = 0.3f;
        c.min_duration_off = 0.5f;
        const auto* sd = api_->SherpaOnnxCreateOfflineSpeakerDiarization(&c);
        if (!sd) return make_error("stt_failed", "Speaker diarization does not start.");
        const auto audio = to_float(t.pcm.data(), t.pcm.size());
        std::vector<Turn> turns;
        if (const auto* r = api_->SherpaOnnxOfflineSpeakerDiarizationProcess(sd, audio.data(), static_cast<std::int32_t>(audio.size()))) {
            const std::int32_t n = api_->SherpaOnnxOfflineSpeakerDiarizationResultGetNumSegments(r);
            if (const auto* segs = n > 0 ? api_->SherpaOnnxOfflineSpeakerDiarizationResultSortByStartTime(r) : nullptr) {
                for (std::int32_t i = 0; i < n; ++i)
                    turns.push_back({static_cast<long long>(segs[i].start * 1000.0f), static_cast<long long>(segs[i].end * 1000.0f), segs[i].speaker});
                api_->SherpaOnnxOfflineSpeakerDiarizationDestroySegment(segs);
            }
            api_->SherpaOnnxOfflineSpeakerDiarizationDestroyResult(r);
        }
        api_->SherpaOnnxDestroyOfflineSpeakerDiarization(sd);
        return turns;  // none: the track is one speaker
    }

    Result<std::string> identify(const std::vector<TrackAudio>& tracks, const std::vector<std::vector<Span>>& spans) const {
        std::vector<std::vector<float>> windows;
        std::vector<size_t> order;
        for (size_t i = 0; i < tracks.size(); ++i)
            if (tracks[i].track == "local") order.insert(order.begin(), i);
            else order.push_back(i);
        for (size_t i : order) {
            std::vector<float> w;
            for (const auto& s : spans[i]) {
                if (static_cast<int>(windows.size()) >= kLanguageVotes) break;
                const size_t take = std::min(s.end - s.begin, kLanguageWindow - w.size());
                const auto f = to_float(tracks[i].pcm.data() + s.begin, take);
                w.insert(w.end(), f.begin(), f.end());
                if (w.size() >= kLanguageMinimum) windows.push_back(std::move(w)), w.clear();
            }
            if (!w.empty() && windows.size() < static_cast<size_t>(kLanguageVotes) && (windows.empty() || w.size() >= kLanguageMinimum / 2))
                windows.push_back(std::move(w));
        }
        if (windows.empty()) return std::string("en");  // no speech: nothing to transcribe in any language
        SherpaOnnxSpokenLanguageIdentificationConfig c{};
        c.whisper.encoder = paths_.whisper_encoder.c_str();
        c.whisper.decoder = paths_.whisper_decoder.c_str();
        c.num_threads = threads_;
        c.provider = "cpu";
        const auto* slid = api_->SherpaOnnxCreateSpokenLanguageIdentification(&c);
        if (!slid) return make_error("stt_failed", "Language identification does not start.");
        std::vector<std::string> votes;
        for (const auto& w : windows) {
            auto* s = api_->SherpaOnnxSpokenLanguageIdentificationCreateOfflineStream(slid);
            api_->SherpaOnnxAcceptWaveformOffline(s, kRate, w.data(), static_cast<std::int32_t>(w.size()));
            if (const auto* r = api_->SherpaOnnxSpokenLanguageIdentificationCompute(slid, s)) {
                votes.push_back(r->lang ? to_lower(r->lang) : "");
                api_->SherpaOnnxDestroySpokenLanguageIdentificationResult(r);
            }
            api_->SherpaOnnxDestroyOfflineStream(s);
        }
        api_->SherpaOnnxDestroySpokenLanguageIdentification(slid);
        const std::string lang = vote_language(votes);
        if (lang.empty()) return make_error("stt_failed", "The language was not identified.");
        return lang;
    }

    Result<std::vector<std::vector<std::string>>> recognize(const std::vector<TrackAudio>& tracks,
                                                            const std::vector<std::vector<Span>>& spans, bool parakeet,
                                                            const std::string& lang) const {
        SherpaOnnxOfflineRecognizerConfig c{};
        c.feat_config.sample_rate = kRate;
        c.feat_config.feature_dim = 80;  // NeMo models take their own feature size from the model metadata
        if (parakeet) {
            c.model_config.transducer.encoder = paths_.parakeet_encoder.c_str();
            c.model_config.transducer.decoder = paths_.parakeet_decoder.c_str();
            c.model_config.transducer.joiner = paths_.parakeet_joiner.c_str();
            c.model_config.tokens = paths_.parakeet_tokens.c_str();
            c.model_config.model_type = "nemo_transducer";
        } else {
            c.model_config.whisper.encoder = paths_.whisper_encoder.c_str();
            c.model_config.whisper.decoder = paths_.whisper_decoder.c_str();
            c.model_config.whisper.language = lang.c_str();
            c.model_config.whisper.task = "transcribe";
            c.model_config.whisper.tail_paddings = -1;
            c.model_config.tokens = paths_.whisper_tokens.c_str();
        }
        c.model_config.num_threads = threads_;
        c.model_config.provider = "cpu";
        c.decoding_method = "greedy_search";
        // The meeting vocabulary as hotwords (improve-meeting-capture-quality D2): Parakeet only, modified beam search
        // with 4 paths at score 2.0, and a BPE vocabulary generated from tokens.txt so sherpa-onnx can encode the terms.
        HotwordFiles hot;
        std::string hot_file, bpe_file;
        if (parakeet && !hotwords_.empty() && hot.write(paths_.parakeet_tokens, hotwords_)) {
            hot_file = narrow(hot.hotwords.wstring());
            bpe_file = narrow(hot.vocab.wstring());
            c.decoding_method = "modified_beam_search";
            c.max_active_paths = 4;
            c.hotwords_file = hot_file.c_str();
            c.hotwords_score = 2.0f;
            c.model_config.modeling_unit = "bpe";
            c.model_config.bpe_vocab = bpe_file.c_str();
        }
        const auto* rec = api_->SherpaOnnxCreateOfflineRecognizer(&c);
        if (!rec && !hot_file.empty()) {
            // Hotwords must never cost the transcript: decode as before without them.
            trace("hotwords refused");
            c.decoding_method = "greedy_search";
            c.max_active_paths = 0;
            c.hotwords_file = nullptr;
            c.hotwords_score = 0;
            c.model_config.modeling_unit = nullptr;
            c.model_config.bpe_vocab = nullptr;
            rec = api_->SherpaOnnxCreateOfflineRecognizer(&c);
        }
        hot.remove();
        if (!rec) return make_error("stt_failed", "The speech recognizer does not start.");
        trace("recognizer loaded");
        int batches = 0;
        std::vector<std::vector<std::string>> out(tracks.size());
        for (size_t i = 0; i < tracks.size(); ++i) {
            out[i].resize(spans[i].size());
            // Decoding units: Parakeet gets spans separated by short pauses together, up to 20 s, so a word or two is
            // never decoded without its context (improve-transcript-language D1). Whisper takes spans one by one.
            std::vector<std::pair<long long, long long>> ms;
            for (const auto& s : spans[i]) ms.emplace_back(to_ms(s.begin), to_ms(s.end));
            std::vector<std::pair<size_t, size_t>> units;
            if (parakeet) units = join_spans(ms, 1000, static_cast<long long>(kBatchSamples / kRate) * 1000);
            else
                for (size_t k = 0; k < spans[i].size(); ++k) units.emplace_back(k, k);
            const auto samples = [&](const std::pair<size_t, size_t>& u) { return spans[i][u.second].end - spans[i][u.first].begin; };
            for (size_t u0 = 0, u1 = 0; u0 < units.size(); u0 = u1) {
                u1 = u0 + 1;
                size_t total = samples(units[u0]);
                while (u1 < units.size() && u1 - u0 < kBatch && total + samples(units[u1]) <= kBatchSamples) total += samples(units[u1]), ++u1;
                std::vector<const SherpaOnnxOfflineStream*> streams;
                for (size_t u = u0; u < u1; ++u) {
                    const size_t b = spans[i][units[u].first].begin, e = spans[i][units[u].second].end;
                    const auto f = to_float(tracks[i].pcm.data() + b, e - b);
                    const auto* st = api_->SherpaOnnxCreateOfflineStream(rec);
                    api_->SherpaOnnxAcceptWaveformOffline(st, kRate, f.data(), static_cast<std::int32_t>(f.size()));
                    streams.push_back(st);
                }
                api_->SherpaOnnxDecodeMultipleOfflineStreams(rec, streams.data(), static_cast<std::int32_t>(streams.size()));
                if (++batches % 50 == 1) trace("asr batch");
                for (size_t u = u0; u < u1; ++u) {
                    const auto [first, last] = units[u];
                    const auto* st = streams[u - u0];
                    if (const auto* r = api_->SherpaOnnxGetOfflineStreamResult(st)) {
                        if (first == last || !r->timestamps || !r->tokens_arr || r->count <= 0) {
                            out[i][first] = trim(r->text ? std::string(r->text) : std::string());
                        } else {
                            // The unit's words back onto its spans by their time.
                            std::vector<std::pair<std::string, float>> tokens;
                            for (std::int32_t t = 0; t < r->count; ++t)
                                tokens.emplace_back(r->tokens_arr[t] ? std::string(r->tokens_arr[t]) : std::string(), r->timestamps[t]);
                            std::vector<std::pair<long long, long long>> rel;
                            for (size_t k = first; k <= last; ++k) rel.emplace_back(ms[k].first - ms[first].first, ms[k].second - ms[first].first);
                            const auto parts = split_by_time(tokens, rel);
                            for (size_t k = first; k <= last; ++k) out[i][k] = trim(parts[k - first]);
                        }
                        api_->SherpaOnnxDestroyOfflineRecognizerResult(r);
                    }
                    api_->SherpaOnnxDestroyOfflineStream(st);
                }
            }
        }
        api_->SherpaOnnxDestroyOfflineRecognizer(rec);
        trace("asr done");
        return out;
    }

    // Segments off the meeting's language, decoded again one by one by Whisper small with the language set; a result
    // that is empty, still off the language, or a known Whisper hallucination makes the segment unclear.
    Status redecode(const std::vector<TrackAudio>& tracks, const std::vector<std::vector<Span>>& spans,
                    std::vector<std::vector<std::string>>& texts, const std::string& lang, std::vector<std::vector<bool>>& unclear) const {
        std::vector<std::pair<size_t, size_t>> todo;
        for (size_t i = 0; i < tracks.size(); ++i)
            for (size_t k = 0; k < spans[i].size(); ++k)
                if (!texts[i][k].empty() && off_language(texts[i][k], lang)) todo.emplace_back(i, k);
        if (todo.empty()) return {};
        trace("redecode start");
        SherpaOnnxOfflineRecognizerConfig c{};
        c.feat_config.sample_rate = kRate;
        c.feat_config.feature_dim = 80;
        c.model_config.whisper.encoder = paths_.whisper_encoder.c_str();
        c.model_config.whisper.decoder = paths_.whisper_decoder.c_str();
        c.model_config.whisper.language = lang.c_str();
        c.model_config.whisper.task = "transcribe";
        c.model_config.whisper.tail_paddings = -1;
        c.model_config.tokens = paths_.whisper_tokens.c_str();
        c.model_config.num_threads = threads_;
        c.model_config.provider = "cpu";
        c.decoding_method = "greedy_search";
        const auto* rec = api_->SherpaOnnxCreateOfflineRecognizer(&c);
        if (!rec) {
            // Without Whisper the segments cannot be checked again: they are unclear rather than foreign.
            for (const auto& [i, k] : todo) unclear[i][k] = true;
            return {};
        }
        for (const auto& [i, k] : todo) {
            const auto& s = spans[i][k];
            const auto f = to_float(tracks[i].pcm.data() + s.begin, s.end - s.begin);
            const auto* st = api_->SherpaOnnxCreateOfflineStream(rec);
            api_->SherpaOnnxAcceptWaveformOffline(st, kRate, f.data(), static_cast<std::int32_t>(f.size()));
            const SherpaOnnxOfflineStream* one[] = {st};
            api_->SherpaOnnxDecodeMultipleOfflineStreams(rec, one, 1);
            std::string text;
            if (const auto* r = api_->SherpaOnnxGetOfflineStreamResult(st)) {
                text = trim(r->text ? std::string(r->text) : std::string());
                api_->SherpaOnnxDestroyOfflineRecognizerResult(r);
            }
            api_->SherpaOnnxDestroyOfflineStream(st);
            if (text.empty() || off_language(text, lang) || whisper_hallucination(text)) unclear[i][k] = true;
            else texts[i][k] = text;
        }
        api_->SherpaOnnxDestroyOfflineRecognizer(rec);
        trace("redecode done");
        return {};
    }

    void set_hotwords(std::vector<std::string> terms) override { hotwords_ = std::move(terms); }

    const Api* api_;
    Paths paths_;
    int threads_;
    bool force_whisper_;
    std::vector<std::string> hotwords_;
};

#ifdef SKOWORK_DEV_ENDPOINTS
std::wstring env(const wchar_t* name);
#endif

// Development builds: SKOWORK_STT_TRACE=<file> appends each stage with the process's private bytes (budget tuning).
void trace(const char* stage) {
#ifdef SKOWORK_DEV_ENDPOINTS
    static const std::wstring file = env(L"SKOWORK_STT_TRACE");
    if (file.empty()) return;
    PROCESS_MEMORY_COUNTERS m{};
    ::GetProcessMemoryInfo(::GetCurrentProcess(), &m, sizeof m);
    if (FILE* f = ::_wfopen(file.c_str(), L"a")) {
        std::fprintf(f, "%llu %s: %llu MB private, peak %llu MB\n", static_cast<unsigned long long>(::GetTickCount64() / 1000), stage,
                     static_cast<unsigned long long>(m.PagefileUsage >> 20), static_cast<unsigned long long>(m.PeakPagefileUsage >> 20));
        std::fclose(f);
    }
#else
    (void)stage;
#endif
}

#ifdef SKOWORK_DEV_ENDPOINTS
std::wstring env(const wchar_t* name) {
    const DWORD n = ::GetEnvironmentVariableW(name, nullptr, 0);
    if (n == 0) return {};
    std::wstring v(n, L'\0');
    v.resize(::GetEnvironmentVariableW(name, v.data(), n));
    return v;
}
#endif

}  // namespace

Result<std::unique_ptr<SpeechEngine>> make_sherpa_engine(const fs::path& runtime_dir, const fs::path& models_dir, int threads,
                                                         bool force_whisper) {
    auto paths = model_paths(models_dir);
    if (!paths) return paths.error();
    auto api = load_api(runtime_dir);
    if (!api) return api.error();
    return std::unique_ptr<SpeechEngine>(std::make_unique<SherpaEngine>(*api, std::move(*paths), std::max(1, threads), force_whisper));
}

}  // namespace wb::meeting
