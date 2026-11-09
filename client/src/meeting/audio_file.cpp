#include "meeting/audio_file.h"

#include "meeting/sherpa_engine.h"

#include "platform/crypto.h"
#include "platform/dpapi.h"
#include "platform/win.h"

#include <algorithm>
#include <cstring>
#include <iterator>
#include <thread>

namespace wb::meeting {
namespace {

constexpr std::wstring_view kPurpose = L"workbench-meeting-audio";

}  // namespace

std::filesystem::path path_from_utf8(const std::string& s) { return std::filesystem::path(widen(s)); }
std::string path_to_utf8(const std::filesystem::path& p) { return narrow(p.wstring()); }

Result<std::unique_ptr<TrackWriter>> TrackWriter::create(const std::filesystem::path& file, TrackKind kind,
                                                         std::uint32_t rate, long long start_ms) {
    std::unique_ptr<TrackWriter> w(new TrackWriter());
    w->key_ = random_bytes(32);
    if (w->key_.size() != 32) return make_error("crypto", "no random key");
    auto wrapped = dpapi_protect(w->key_, kPurpose);
    if (!wrapped) return wrapped.error();
    w->rate_ = rate;
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    w->out_.open(file, std::ios::binary | std::ios::trunc);
    if (!w->out_) return make_error("io", "cannot create the audio file");
    const auto header = encode_header({kind, rate, start_ms, *wrapped});
    w->out_.write(reinterpret_cast<const char*>(header.data()), static_cast<std::streamsize>(header.size()));
    w->out_.flush();
    if (!w->out_) return make_error("io", "cannot write the audio file");
    return w;
}

TrackWriter::~TrackWriter() { (void)close(); }

Status TrackWriter::flush_chunk() {
    if (pending_.empty()) return {};
    ContainerChunk c;
    c.offset = written_;
    c.nonce = random_bytes(12);
    auto sealed = aes_gcm_seal(key_, c.nonce, reinterpret_cast<const std::uint8_t*>(pending_.data()),
                               pending_.size() * sizeof(std::int16_t), chunk_aad(c.offset));
    if (!sealed) return sealed.error();
    c.sealed = std::move(*sealed);
    const auto b = encode_chunk(c);
    out_.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
    out_.flush();
    if (!out_) return make_error("io", "cannot write the audio file");
    written_ += pending_.size();
    pending_.clear();
    return {};
}

Status TrackWriter::append(const std::int16_t* pcm, size_t n) {
    const std::lock_guard lock(mx_);
    if (closed_) return make_error("closed", "the track is closed");
    for (size_t i = 0; i < n;) {
        const size_t take = std::min(n - i, rate_ - pending_.size());
        pending_.insert(pending_.end(), pcm + i, pcm + i + take);
        i += take;
        if (pending_.size() >= rate_)
            if (Status s = flush_chunk(); !s) return s;
    }
    return {};
}

Status TrackWriter::append_silence(size_t n) {
    const std::vector<std::int16_t> zeros(std::min<size_t>(n, 16000u * 60), 0);
    for (size_t done = 0; done < n;) {
        const size_t k = std::min(zeros.size(), n - done);
        if (Status s = append(zeros.data(), k); !s) return s;
        done += k;
    }
    return {};
}

Status TrackWriter::close() {
    const std::lock_guard lock(mx_);
    if (closed_) return {};
    closed_ = true;
    Status s = flush_chunk();
    out_.close();
    std::fill(key_.begin(), key_.end(), 0);
    return s;
}

std::uint64_t TrackWriter::samples() const {
    const std::lock_guard lock(mx_);
    return written_ + pending_.size();
}

Result<DecodedTrack> read_track(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return make_error("io", "cannot open the audio file");
    const std::vector<std::uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    auto parsed = parse_container(data.data(), data.size());
    if (!parsed) return parsed.error();
    auto key = dpapi_unprotect(parsed->header.wrapped_key, kPurpose);
    if (!key || key->size() != 32) return make_error("crypto", "the track key does not open");
    DecodedTrack out{parsed->header, {}, parsed->truncated};
    for (const auto& c : parsed->chunks) {
        auto plain = aes_gcm_open(*key, c.nonce, c.sealed.data(), c.sealed.size(), chunk_aad(c.offset));
        if (!plain) return plain.error();
        if (c.offset > out.pcm.size() + 16000ull * 3600 * 4) return make_error("corrupt", "a chunk lies beyond four hours of silence");
        if (c.offset > out.pcm.size()) out.pcm.resize(static_cast<size_t>(c.offset), 0);
        const size_t k = plain->size() / sizeof(std::int16_t);
        const size_t at = static_cast<size_t>(c.offset);
        if (out.pcm.size() < at + k) out.pcm.resize(at + k, 0);
        std::memcpy(out.pcm.data() + at, plain->data(), k * sizeof(std::int16_t));
    }
    std::fill(key->begin(), key->end(), 0);
    return out;
}

Result<json> stt_request(const std::string& method, const json& params) {
    if (method != "stt.transcribe") return make_error("unknown_method", "the stt worker has no method " + method);
    const json* paths = jobj(params, "tracks");
    if (!paths || !paths->is_array() || paths->empty()) return make_error("invalid_params", "tracks is a list of files");
    std::unique_ptr<SpeechEngine> engine;
#ifdef WORKBENCH_DEV_ENDPOINTS
    // Development builds keep the deterministic test engine unless a request or WORKBENCH_STT_ENGINE asks for sherpa-onnx.
    std::string which = jstr(params, "engine");
    if (which.empty()) {
        wchar_t v[32] = {};
        which = ::GetEnvironmentVariableW(L"WORKBENCH_STT_ENGINE", v, 32) > 0 ? narrow(v) : "test";
    }
    if (which == "test") engine = std::make_unique<TestEngine>();
#endif
    if (!engine) {
        // Half the cores (Transcription Resource Budget); the worker's Job also caps it at half the machine.
        const int threads = static_cast<int>(std::max(1u, std::thread::hardware_concurrency() / 2));
        bool force_whisper = false;
#ifdef WORKBENCH_DEV_ENDPOINTS
        force_whisper = jstr(params, "asr") == "whisper";
#endif
        auto e = make_sherpa_engine(stt_runtime_dir(), stt_models_dir(), threads, force_whisper);
        if (!e) return e.error();
        engine = std::move(*e);
    }
    std::vector<TrackAudio> tracks;
    for (const auto& p : *paths) {
        if (!p.is_string()) return make_error("invalid_params", "tracks is a list of files");
        auto t = read_track(path_from_utf8(p.get<std::string>()));
        if (!t) return t.error();
        tracks.push_back({to_string(t->header.kind), std::move(t->pcm), t->header.sample_rate});
    }
    auto r = engine->transcribe(tracks, jstr(params, "language"));
    if (!r) return r;
    // Dictation (add-dictation D5): a short dictation detected as an unsupported language is transcribed in the
    // fallback language (the window's) instead.
    if (const std::string lang = jstr(*r, "language"), fallback = jstr(params, "fallback_language");
        jstr(params, "language").empty() && !supported_language(lang) && supported_language(fallback)) {
        r = engine->transcribe(tracks, fallback);
        if (!r) return r;
    }
    if (const std::string lang = jstr(*r, "language"); !supported_language(lang))
        return make_error("unsupported_language", lang);
    return r;
}

}  // namespace wb::meeting
