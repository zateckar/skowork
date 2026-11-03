// client/meeting-capture (add-meeting-capture), the portable part: policy configuration, the meeting app list and its
// matching against the process tree, the calendar match, participant-notice texts, the audio container framing (design
// D2), transcript segments with speaker naming and Markdown, an energy VAD and the development test engine (design D4),
// the meeting Trigger conditions (Meeting Trigger Conditions) and the detector's state machine (design D3). The fuzz job
// and the stt worker build it; nothing here touches audio devices, files or keys.
#pragma once

#include "base/json.h"
#include "base/result.h"

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wb::meeting {

// ---- policy.meeting_capture ----

struct AppEntry {
    std::string process;     // image name, compared case-insensitively ("ms-teams.exe")
    std::string title_glob;  // empty: any window; `*` matches any text
};

struct Config {
    bool enabled = false;
    std::vector<AppEntry> apps;
    bool allow_manual = false;
    bool allow_series_auto = false;
    bool live_transcript = false;
    bool keep_audio = false;
    int max_pending_days = 7;
    bool require_notice = true;
    std::map<std::string, std::string> notice_text;  // language -> text; built-in cs, de, en filled in
};

using PolicyLookup = std::function<std::optional<json>(const std::string& key)>;
// Reads policy.meeting_capture.*; a malformed value fails ("policy") and capture stays off.
Result<Config> parse_config(const PolicyLookup& policy);
std::optional<AppEntry> parse_app_entry(const std::string& entry);
const std::vector<AppEntry>& default_apps();
// Case-insensitive; `*` matches any run of characters.
bool glob_match(std::string_view pattern, std::string_view text);

// What a capture records: a process tree (process loopback), all system output, or the microphone.
struct CaptureSource {
    enum class Kind { process, system, mic } kind = Kind::process;
    unsigned pid = 0;  // process: the root of the tree
};

// ---- detection ----

struct Process {
    unsigned pid = 0, parent = 0;
    std::string name;  // image name
};

struct AppMatch {
    size_t entry = 0;     // index in Config::apps
    unsigned root = 0;    // the process with the entry's name: capture targets its tree
    std::string process;  // its image name
    std::string title;    // the matching window title (entries with a pattern)
};

// The entry an audio session of `pid` belongs to: its process or a parent has the entry's name and, for an entry with
// a pattern, one of that process's windows has a matching title. `titles(pid)` gives a process's top-level window titles.
std::optional<AppMatch> match_app(const std::vector<AppEntry>& apps, unsigned pid, const std::vector<Process>& processes,
                                  const std::function<std::vector<std::string>(unsigned pid)>& titles);

struct CalendarEvent {
    std::string id, series_id, subject, online_url, organizer;
    long long start = 0, end = 0;  // unix ms
    std::vector<std::string> attendees;  // names (or addresses when the name is unknown), the user excluded
};

// The closest event with an online-meeting link whose start is within 10 minutes of `now`, or that is under way.
std::optional<CalendarEvent> match_calendar(const std::vector<CalendarEvent>& events, long long now);

// One observation of the machine (every 2 seconds while capture is enabled).
struct Observation {
    std::vector<unsigned> session_pids;  // processes with an active render session
    bool mic_in_use = false;
    std::vector<Process> processes;
    std::function<std::vector<std::string>(unsigned pid)> titles;
};

struct Detection {
    std::string kind;  // "detected" | "ended"
    AppMatch app;
};

// A meeting starts when a listed app has an active session while the microphone is in use, and ends after 30 seconds
// without an active session of that app. One meeting per app entry at a time.
class Detector {
public:
    explicit Detector(std::vector<AppEntry> apps) : apps_(std::move(apps)) {}
    std::vector<Detection> observe(const Observation& o, long long now);
    void reset() { live_.clear(); }
    [[nodiscard]] bool active(unsigned root) const;

private:
    struct Live {
        AppMatch app;
        long long last_seen = 0;
    };
    std::vector<AppEntry> apps_;
    std::vector<Live> live_;
};

// ---- participant notice ----

const std::map<std::string, std::string>& builtin_notices();
// The notice in the meeting language followed by the English one (once when the language is English or unknown).
std::string notice_for(const Config& c, const std::string& language);

// ---- audio container (design D2) ----
// Header: "SKAU", u16 version (1), u8 track kind, u8 reserved, u32 sample rate, i64 start (unix ms), u16 wrapped-key
// length, the DPAPI-wrapped key. Chunks: u64 sample offset, 12-byte nonce, u32 sealed length, the sealed PCM (s16 mono
// little-endian, AES-256-GCM with the offset as associated data). All integers little-endian.

enum class TrackKind : std::uint8_t { remote = 1, local = 2, system = 3, mic = 4 };
const char* to_string(TrackKind k);
std::optional<TrackKind> track_kind(std::string_view s);

struct ContainerHeader {
    TrackKind kind = TrackKind::remote;
    std::uint32_t sample_rate = 16000;
    long long start_ms = 0;
    std::vector<std::uint8_t> wrapped_key;
};

struct ContainerChunk {
    std::uint64_t offset = 0;  // in samples
    std::vector<std::uint8_t> nonce;
    std::vector<std::uint8_t> sealed;
};

inline constexpr size_t kMaxChunkBytes = 1u << 20;  // a sealed chunk is about 32 KB; anything above 1 MB is corrupt

std::vector<std::uint8_t> encode_header(const ContainerHeader& h);
std::vector<std::uint8_t> encode_chunk(const ContainerChunk& c);
// The associated data of a chunk: its sample offset as 8 little-endian bytes.
std::string chunk_aad(std::uint64_t offset);
struct ParsedContainer {
    ContainerHeader header;
    std::vector<ContainerChunk> chunks;
    bool truncated = false;  // the last chunk was cut off (a capture that ended abruptly); the rest is usable
};
// Fails ("corrupt") on a bad header, a chunk over kMaxChunkBytes or offsets that go backwards.
Result<ParsedContainer> parse_container(const std::uint8_t* data, size_t n);

// ---- transcripts ----

struct Segment {
    std::string track;    // remote | local | system | mic
    long long start_ms = 0, end_ms = 0;  // from the start of the capture
    std::string speaker;  // "me" for the local track, "s1", "s2", ... for clusters of the other tracks
    std::string text;
};

// The worker's result: {language, segments: [{track, start_ms, end_ms, speaker, text}]}. Fails ("invalid") when the
// shape is wrong; segments come back sorted by start time.
Result<std::vector<Segment>> parse_segments(const json& j);
json segments_json(const std::vector<Segment>& segments);

// Speaker names: "me" is the user; remote clusters get "Speaker N", or the attendee's name when the meeting has exactly
// one other attendee and one remote cluster (a confident match). `existing` keeps names the user set.
std::map<std::string, std::string> name_speakers(const std::vector<Segment>& segments, const std::string& user_name,
                                                 const std::vector<std::string>& attendees,
                                                 const std::map<std::string, std::string>& existing = {});
// "[mm:ss] Name: text" lines, one per segment.
std::string transcript_markdown(const std::vector<Segment>& segments, const std::map<std::string, std::string>& speakers);
// Turns for client/knowledge-graph: [{speaker, start_ms, text}] with the speakers' names.
json knowledge_turns(const std::vector<Segment>& segments, const std::map<std::string, std::string>& speakers);

// Languages transcription supports (Local CPU Transcription).
bool supported_language(const std::string& code);
// Languages Parakeet-TDT-0.6B-v3 transcribes (design D1); a supported language outside them goes to Whisper small.
bool parakeet_language(const std::string& code);
// The most frequent code among the votes (a tie goes to the one seen first); empty when there are none.
std::string vote_language(const std::vector<std::string>& votes);

// A diarization turn of one cluster on a track.
struct Turn {
    long long start_ms = 0, end_ms = 0;
    int cluster = 0;
};
// Labels each speech span of a track with the cluster whose turns overlap it most (the nearest turn when none
// overlaps) as "s<next>", "s<next+1>", ... in order of first appearance; `next` advances past the labels used, so
// several tracks number their speakers in one sequence. Without turns every span is "s<next>".
std::vector<std::string> label_spans(const std::vector<std::pair<long long, long long>>& spans_ms,
                                     const std::vector<Turn>& turns, int& next);

// ---- speech (design D4) ----

struct Span {
    size_t begin = 0, end = 0;  // samples
    double rms = 0;
};
// Energy VAD over 30 ms frames: speech where the frame RMS is above `threshold` (16-bit full scale), joined across
// gaps under 300 ms; spans under 200 ms are dropped.
std::vector<Span> energy_vad(const std::int16_t* pcm, size_t n, std::uint32_t rate, double threshold = 500);

struct TrackAudio {
    std::string track;
    std::vector<std::int16_t> pcm;
    std::uint32_t rate = 16000;
};

// The engine behind the stt worker. The sherpa-onnx engine needs the models package (design D1); without it the
// worker reports model_missing.
class SpeechEngine {
public:
    virtual ~SpeechEngine() = default;
    // language: a code, or empty to detect it. Returns {language, segments}.
    virtual Result<json> transcribe(const std::vector<TrackAudio>& tracks, const std::string& language) = 0;
};

// Development builds: every VAD span becomes "Segment N" on its track; remote spans are split into two speakers by
// loudness. The language is the requested one, else "en".
class TestEngine : public SpeechEngine {
public:
    Result<json> transcribe(const std::vector<TrackAudio>& tracks, const std::string& language) override;
};

// ---- Trigger kind meeting (Meeting Trigger Conditions) ----

// {event?: "detected"|"transcript_ready", app?, title_contains?}
Status validate_meeting_conditions(const json& conditions);
// `meeting`: {event, app, title}.
bool meeting_matches(const json& conditions, const json& meeting);
// What a firing's Run gets: ids only.
json meeting_trigger_detail(const std::string& event, const std::string& meeting_id, const std::string& thread_id,
                            const std::string& calendar_event_id);

}  // namespace wb::meeting
