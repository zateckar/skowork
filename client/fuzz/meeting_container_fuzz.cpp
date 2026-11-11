// Fuzz target for the meeting core (client/meeting-capture; add-meeting-capture design D2): the audio container parser,
// the worker's transcript segments, speaker naming and Markdown, the energy VAD, app globs and meeting Trigger conditions
// must give an answer for any input, never a crash; a parsed container re-encodes to one that parses the same.
#include "fuzz_main.h"
#include "meeting/core.h"

#include <cstring>

using namespace wb;
using namespace wb::meeting;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    // The container.
    if (const auto c = parse_container(data, size)) {
        if (c->header.sample_rate < 8000 || c->header.sample_rate > 48000) __builtin_trap();
        std::vector<std::uint8_t> again = encode_header(c->header);
        std::uint64_t last = 0;
        bool first = true;
        for (const auto& ch : c->chunks) {
            if (ch.sealed.size() > kMaxChunkBytes || ch.nonce.size() != 12) __builtin_trap();
            if (!first && ch.offset <= last) __builtin_trap();
            first = false;
            last = ch.offset;
            const auto b = encode_chunk(ch);
            again.insert(again.end(), b.begin(), b.end());
        }
        const auto r = parse_container(again.data(), again.size());
        if (!r || r->chunks.size() != c->chunks.size() || r->truncated || r->header.kind != c->header.kind) __builtin_trap();
    }

    // PCM through the VAD.
    const size_t samples = size / 2;
    std::vector<std::int16_t> pcm(samples);
    if (samples) std::memcpy(pcm.data(), data, samples * 2);
    for (const auto& s : energy_vad(pcm.data(), pcm.size(), 16000, 200))
        if (s.begin > s.end || s.end > pcm.size()) __builtin_trap();

    // The worker's result and the conditions as JSON.
    const std::string text(reinterpret_cast<const char*>(data), size);
    const json j = json::parse(text, nullptr, false);
    if (!j.is_discarded()) {
        if (const auto segs = parse_segments(j)) {
            for (size_t i = 1; i < segs->size(); ++i)
                if ((*segs)[i].start_ms < (*segs)[i - 1].start_ms) __builtin_trap();
            const auto names = name_speakers(*segs, "Jan Novák", {"Eva"});
            for (const auto& s : *segs)
                if (!names.count(s.speaker)) __builtin_trap();
            (void)json(transcript_markdown(*segs, names)).dump();
            (void)knowledge_turns(*segs, names).dump();
            if (!parse_segments({{"segments", segments_json(*segs)}})) __builtin_trap();
            // improve-meeting-capture-quality: echo marking only ever hides local segments.
            auto marked = *segs;
            const size_t n = mark_echo(marked);
            size_t hidden = 0;
            for (const auto& s : marked) {
                if (s.echo && s.track != "local") __builtin_trap();
                hidden += s.echo ? 1 : 0;
            }
            if (hidden != n || heard(marked).size() != marked.size() - n) __builtin_trap();
        }
        if (validate_meeting_conditions(j)) (void)meeting_matches(j, {{"event", "detected"}, {"app", "ms-teams.exe"}, {"title", text}});
        (void)parse_config([&j](const std::string& k) -> std::optional<json> {
            if (j.is_object() && j.contains(k)) return j[k];
            return std::nullopt;
        });
    }

    // The first line is an app entry, the rest a window title.
    const size_t nl = text.find('\n');
    if (nl != std::string::npos) {
        if (const auto e = parse_app_entry(text.substr(0, nl))) (void)glob_match(e->title_glob, std::string_view(text).substr(nl + 1));
        // The first line as vocabulary terms and an address, the rest as transcript text (any bytes, not only UTF-8).
        const std::vector<std::string> vocabulary = meeting_terms({text.substr(0, nl)}, {text.substr(0, nl)});
        (void)organisation_of(text.substr(0, nl));
        (void)respell(std::string_view(text).substr(nl + 1), vocabulary);
        (void)respell(std::string_view(text).substr(nl + 1), {text.substr(0, nl)});
    }
    (void)echo_tokens(text);
    // Two halves as the microphone and the remote track.
    if (samples >= 4) {
        const std::vector<std::int16_t> a(pcm.begin(), pcm.begin() + static_cast<std::ptrdiff_t>(samples / 2)), b(pcm.begin() + static_cast<std::ptrdiff_t>(samples / 2), pcm.end());
        (void)echo_evidence(a, b, 400, {{0, 100}, {50, 30}, {-5, 1'000'000}});
    }
    {  // Track parts from the input's numbers.
        std::vector<TrackPart> parts;
        for (size_t i = 0; i + 3 <= size && parts.size() < 4; i += 3)
            parts.push_back({(data[i] & 1) ? "local" : "remote", static_cast<long long>(data[i + 1]) * 100 - 5000,
                             std::vector<std::int16_t>(data[i + 2], 1), 16000});
        (void)join_parts(std::move(parts));
    }
    return 0;
}