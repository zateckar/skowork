#include "meeting/core.h"

#include "base/strings.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>

namespace wb::meeting {
namespace {

constexpr long long kMinute = 60'000;

Error policy_error(const std::string& key, const std::string& what) {
    return make_error("policy", "policy.meeting_capture." + key + " " + what);
}

std::optional<bool> read_bool(const PolicyLookup& p, const std::string& key, bool fallback, std::optional<Error>& err) {
    const auto v = p("meeting_capture." + key);
    if (!v) return fallback;
    if (!v->is_boolean()) {
        err = policy_error(key, "is true or false");
        return std::nullopt;
    }
    return v->get<bool>();
}

void put_u16(std::vector<std::uint8_t>& b, std::uint16_t v) {
    b.push_back(static_cast<std::uint8_t>(v & 0xff));
    b.push_back(static_cast<std::uint8_t>(v >> 8));
}
void put_u32(std::vector<std::uint8_t>& b, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xff));
}
void put_u64(std::vector<std::uint8_t>& b, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) b.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xff));
}

struct Reader {
    const std::uint8_t* p;
    size_t n, at = 0;
    [[nodiscard]] bool has(size_t k) const { return n - at >= k; }
    std::uint64_t le(size_t k) {
        std::uint64_t v = 0;
        for (size_t i = 0; i < k; ++i) v |= static_cast<std::uint64_t>(p[at + i]) << (8 * i);
        at += k;
        return v;
    }
    std::vector<std::uint8_t> take(size_t k) {
        std::vector<std::uint8_t> v(p + at, p + at + k);
        at += k;
        return v;
    }
};

std::string mmss(long long ms) {
    const long long s = std::max(0LL, ms) / 1000;
    char buf[32];
    if (s >= 3600) std::snprintf(buf, sizeof buf, "%lld:%02lld:%02lld", s / 3600, (s / 60) % 60, s % 60);
    else std::snprintf(buf, sizeof buf, "%02lld:%02lld", s / 60, s % 60);
    return buf;
}

bool contains_folded(const std::string& haystack, const std::string& needle) {
    return needle.empty() || fold_latin(to_lower(haystack)).find(fold_latin(to_lower(needle))) != std::string::npos;
}

}  // namespace

// ---- configuration ---------------------------------------------------------------------------------------------------

const std::vector<AppEntry>& default_apps() {
    static const std::vector<AppEntry> apps = {
        {"ms-teams.exe", ""},          {"Teams.exe", ""},          {"Zoom.exe", ""},
        {"chrome.exe", "*Jitsi Meet*"}, {"chrome.exe", "Meet - *"}, {"msedge.exe", "*Jitsi Meet*"},
        {"msedge.exe", "Meet - *"},
    };
    return apps;
}

std::optional<AppEntry> parse_app_entry(const std::string& entry) {
    const size_t bar = entry.find('|');
    AppEntry e{trim(entry.substr(0, bar)), bar == std::string::npos ? std::string{} : trim(entry.substr(bar + 1))};
    if (e.process.empty() || e.process.find_first_of("\\/:") != std::string::npos) return std::nullopt;
    return e;
}

const std::map<std::string, std::string>& builtin_notices() {
    static const std::map<std::string, std::string> n = {
        {"en", "Heads-up: I am recording the audio of this meeting with SkoWork to create a transcript and notes for "
               "myself. The recording stays on my computer and is deleted after transcription. Tell me if you object."},
        {"cs", "Upozornění: zvuk této schůzky nahrávám pomocí SkoWork kvůli přepisu a poznámkám pro vlastní potřebu. "
               "Nahrávka zůstává v mém počítači a po přepisu se smaže. Pokud nesouhlasíte, dejte mi prosím vědět."},
        {"de", "Hinweis: Ich zeichne den Ton dieser Besprechung mit SkoWork auf, um ein Transkript und Notizen für mich "
               "zu erstellen. Die Aufnahme bleibt auf meinem Computer und wird nach der Transkription gelöscht. Sagen Sie "
               "mir bitte, wenn Sie nicht einverstanden sind."},
    };
    return n;
}

Result<Config> parse_config(const PolicyLookup& policy) {
    Config c;
    std::optional<Error> err;
    const auto flag = [&](const char* key, bool fallback) { return read_bool(policy, key, fallback, err).value_or(fallback); };
    c.enabled = flag("enabled", false);
    c.allow_manual = flag("allow_manual", false);
    c.allow_series_auto = flag("allow_series_auto", false);
    c.live_transcript = flag("live_transcript", false);
    c.keep_audio = flag("keep_audio", false);
    c.require_notice = flag("require_notice", true);
    if (err) return *err;
    if (const auto v = policy("meeting_capture.max_pending_days")) {
        if (!v->is_number_integer() || v->get<long long>() < 1 || v->get<long long>() > 365)
            return policy_error("max_pending_days", "is a whole number of days from 1 to 365");
        c.max_pending_days = static_cast<int>(v->get<long long>());
    }
    if (const auto v = policy("meeting_capture.apps")) {
        if (!v->is_array()) return policy_error("apps", "is a list");
        for (const auto& x : *v) {
            const auto e = x.is_string() ? parse_app_entry(x.get<std::string>()) : std::nullopt;
            if (!e) return policy_error("apps", "has an entry that is not process.exe or process.exe|title-pattern");
            c.apps.push_back(*e);
        }
    } else {
        c.apps = default_apps();
    }
    c.notice_text = builtin_notices();
    if (const auto v = policy("meeting_capture.notice_text")) {
        if (!v->is_object()) return policy_error("notice_text", "is a map from language to text");
        for (const auto& [lang, text] : v->items()) {
            if (!text.is_string() || trim(text.get<std::string>()).empty()) return policy_error("notice_text", "has an empty text");
            c.notice_text[to_lower(lang)] = text.get<std::string>();
        }
    }
    return c;
}

bool glob_match(std::string_view pattern, std::string_view text) {
    // Iterative wildcard match with backtracking to the last star; ASCII case folding.
    const auto lower = [](char ch) { return ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch - 'A' + 'a') : ch; };
    size_t p = 0, t = 0, star = std::string_view::npos, mark = 0;
    while (t < text.size()) {
        if (p < pattern.size() && pattern[p] == '*') {
            star = p++;
            mark = t;
        } else if (p < pattern.size() && lower(pattern[p]) == lower(text[t])) {
            ++p;
            ++t;
        } else if (star != std::string_view::npos) {
            p = star + 1;
            t = ++mark;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*') ++p;
    return p == pattern.size();
}

// ---- detection -------------------------------------------------------------------------------------------------------

std::optional<AppMatch> match_app(const std::vector<AppEntry>& apps, unsigned pid, const std::vector<Process>& processes,
                                  const std::function<std::vector<std::string>(unsigned)>& titles) {
    std::map<unsigned, const Process*> by_pid;
    for (const auto& p : processes) by_pid[p.pid] = &p;
    // The process and its ancestors, nearest first (a cycle or a reused pid stops the walk).
    std::vector<const Process*> chain;
    std::set<unsigned> seen;
    for (unsigned cur = pid; cur && !seen.count(cur);) {
        seen.insert(cur);
        const auto it = by_pid.find(cur);
        if (it == by_pid.end()) break;
        chain.push_back(it->second);
        cur = it->second->parent;
    }
    for (size_t i = 0; i < apps.size(); ++i) {
        const AppEntry& e = apps[i];
        // The outermost ancestor with the entry's name is the root of the meeting app's tree.
        const Process* root = nullptr;
        for (const Process* p : chain)
            if (iequals(p->name, e.process)) root = p;
        if (!root) continue;
        AppMatch m{i, root->pid, root->name, {}};
        if (e.title_glob.empty()) return m;
        if (!titles) continue;
        // The browser's windows belong to the root process; a renderer child playing the audio has none.
        for (const Process* p : chain) {
            if (!iequals(p->name, e.process)) continue;
            for (const auto& t : titles(p->pid))
                if (glob_match(e.title_glob, t)) {
                    m.title = t;
                    return m;
                }
        }
    }
    return std::nullopt;
}

std::optional<CalendarEvent> match_calendar(const std::vector<CalendarEvent>& events, long long now) {
    const CalendarEvent* best = nullptr;
    long long best_d = 0;
    for (const auto& e : events) {
        if (e.online_url.empty()) continue;
        const bool under_way = e.start <= now && now < e.end;
        const long long d = std::llabs(e.start - now);
        if (!under_way && d > 10 * kMinute) continue;
        if (!best || d < best_d) {
            best = &e;
            best_d = d;
        }
    }
    return best ? std::optional<CalendarEvent>(*best) : std::nullopt;
}

bool Detector::active(unsigned root) const {
    return std::any_of(live_.begin(), live_.end(), [&](const Live& l) { return l.app.root == root; });
}

std::vector<Detection> Detector::observe(const Observation& o, long long now) {
    std::vector<Detection> out;
    std::vector<AppMatch> seen;
    for (const unsigned pid : o.session_pids)
        if (auto m = match_app(apps_, pid, o.processes, o.titles))
            if (std::none_of(seen.begin(), seen.end(), [&](const AppMatch& s) { return s.entry == m->entry && s.root == m->root; }))
                seen.push_back(*m);
    for (const auto& m : seen) {
        auto it = std::find_if(live_.begin(), live_.end(), [&](const Live& l) { return l.app.entry == m.entry && l.app.root == m.root; });
        if (it != live_.end()) {
            it->last_seen = now;
            continue;
        }
        if (!o.mic_in_use) continue;  // audio without the microphone is not a call
        live_.push_back({m, now});
        out.push_back({"detected", m});
    }
    for (auto it = live_.begin(); it != live_.end();) {
        if (now - it->last_seen >= 30'000) {
            out.push_back({"ended", it->app});
            it = live_.erase(it);
        } else {
            ++it;
        }
    }
    return out;
}

// ---- participant notice ----------------------------------------------------------------------------------------------

std::string notice_for(const Config& c, const std::string& language) {
    const auto text = [&](const std::string& lang) {
        const auto it = c.notice_text.find(lang);
        if (it != c.notice_text.end()) return it->second;
        const auto b = builtin_notices().find(lang);
        return b != builtin_notices().end() ? b->second : std::string{};
    };
    const std::string lang = to_lower(language);
    const std::string en = text("en");
    const std::string local = lang.empty() || lang == "en" ? std::string{} : text(lang);
    if (local.empty()) return en;
    return local + "\n\n" + en;
}

// ---- audio container -------------------------------------------------------------------------------------------------

const char* to_string(TrackKind k) {
    switch (k) {
    case TrackKind::remote: return "remote";
    case TrackKind::local: return "local";
    case TrackKind::system: return "system";
    case TrackKind::mic: return "mic";
    }
    return "remote";
}

std::optional<TrackKind> track_kind(std::string_view s) {
    if (s == "remote") return TrackKind::remote;
    if (s == "local") return TrackKind::local;
    if (s == "system") return TrackKind::system;
    if (s == "mic") return TrackKind::mic;
    return std::nullopt;
}

std::vector<std::uint8_t> encode_header(const ContainerHeader& h) {
    std::vector<std::uint8_t> b = {'S', 'K', 'A', 'U'};
    put_u16(b, 1);
    b.push_back(static_cast<std::uint8_t>(h.kind));
    b.push_back(0);
    put_u32(b, h.sample_rate);
    put_u64(b, static_cast<std::uint64_t>(h.start_ms));
    put_u16(b, static_cast<std::uint16_t>(h.wrapped_key.size()));
    b.insert(b.end(), h.wrapped_key.begin(), h.wrapped_key.end());
    return b;
}

std::vector<std::uint8_t> encode_chunk(const ContainerChunk& c) {
    std::vector<std::uint8_t> b;
    b.reserve(24 + c.sealed.size());
    put_u64(b, c.offset);
    b.insert(b.end(), c.nonce.begin(), c.nonce.end());
    put_u32(b, static_cast<std::uint32_t>(c.sealed.size()));
    b.insert(b.end(), c.sealed.begin(), c.sealed.end());
    return b;
}

std::string chunk_aad(std::uint64_t offset) {
    std::string s(8, '\0');
    for (int i = 0; i < 8; ++i) s[static_cast<size_t>(i)] = static_cast<char>((offset >> (8 * i)) & 0xff);
    return s;
}

Result<ParsedContainer> parse_container(const std::uint8_t* data, size_t n) {
    Reader r{data, n};
    if (!r.has(22) || data[0] != 'S' || data[1] != 'K' || data[2] != 'A' || data[3] != 'U')
        return make_error("corrupt", "not an audio container");
    r.at = 4;
    if (r.le(2) != 1) return make_error("corrupt", "unsupported container version");
    ParsedContainer out;
    const auto kind = static_cast<std::uint8_t>(r.le(1));
    if (kind < 1 || kind > 4) return make_error("corrupt", "unknown track kind");
    out.header.kind = static_cast<TrackKind>(kind);
    r.le(1);
    out.header.sample_rate = static_cast<std::uint32_t>(r.le(4));
    if (out.header.sample_rate < 8000 || out.header.sample_rate > 48000) return make_error("corrupt", "bad sample rate");
    out.header.start_ms = static_cast<long long>(r.le(8));
    const size_t klen = static_cast<size_t>(r.le(2));
    if (klen == 0 || !r.has(klen)) return make_error("corrupt", "bad key");
    out.header.wrapped_key = r.take(klen);
    std::uint64_t last = 0;
    while (r.at < n) {
        if (!r.has(24)) {
            out.truncated = true;
            break;
        }
        ContainerChunk c;
        c.offset = r.le(8);
        c.nonce = r.take(12);
        const size_t len = static_cast<size_t>(r.le(4));
        if (len < 16 || len > kMaxChunkBytes) return make_error("corrupt", "bad chunk length");
        if (!out.chunks.empty() && c.offset <= last) return make_error("corrupt", "chunk offsets go backwards");
        if (!r.has(len)) {
            out.truncated = true;
            break;
        }
        c.sealed = r.take(len);
        last = c.offset;
        out.chunks.push_back(std::move(c));
    }
    return out;
}

// ---- transcripts -----------------------------------------------------------------------------------------------------

Result<std::vector<Segment>> parse_segments(const json& j) {
    const json* segs = jobj(j, "segments");
    if (!segs || !segs->is_array()) return make_error("invalid", "the transcription result has no segments");
    std::vector<Segment> out;
    for (const auto& s : *segs) {
        if (!s.is_object()) return make_error("invalid", "a segment is not an object");
        Segment g{jstr(s, "track"), jint(s, "start_ms", -1), jint(s, "end_ms", -1), jstr(s, "speaker"), jstr(s, "text")};
        if (!track_kind(g.track)) return make_error("invalid", "a segment has an unknown track");
        if (g.start_ms < 0 || g.end_ms < g.start_ms) return make_error("invalid", "a segment has bad times");
        if (g.speaker.empty()) g.speaker = g.track == "local" ? "me" : "s1";
        if (g.speaker.size() > 32) return make_error("invalid", "a speaker id is too long");
        if (trim(g.text).empty()) continue;
        out.push_back(std::move(g));
    }
    std::stable_sort(out.begin(), out.end(), [](const Segment& a, const Segment& b) { return a.start_ms < b.start_ms; });
    return out;
}

json segments_json(const std::vector<Segment>& segments) {
    json a = json::array();
    for (const auto& s : segments)
        a.push_back({{"track", s.track}, {"start_ms", s.start_ms}, {"end_ms", s.end_ms}, {"speaker", s.speaker}, {"text", s.text}});
    return a;
}

std::map<std::string, std::string> name_speakers(const std::vector<Segment>& segments, const std::string& user_name,
                                                 const std::vector<std::string>& attendees,
                                                 const std::map<std::string, std::string>& existing) {
    std::vector<std::string> clusters;
    bool has_me = false;
    for (const auto& s : segments) {
        if (s.speaker == "me") has_me = true;
        else if (std::find(clusters.begin(), clusters.end(), s.speaker) == clusters.end()) clusters.push_back(s.speaker);
    }
    std::map<std::string, std::string> out;
    if (has_me) out["me"] = user_name.empty() ? "Me" : user_name;
    for (size_t i = 0; i < clusters.size(); ++i) out[clusters[i]] = "Speaker " + std::to_string(i + 1);
    if (clusters.size() == 1 && attendees.size() == 1 && !trim(attendees[0]).empty()) out[clusters[0]] = attendees[0];
    for (const auto& [id, name] : existing)
        if (out.count(id)) out[id] = name;
    return out;
}

std::string transcript_markdown(const std::vector<Segment>& segments, const std::map<std::string, std::string>& speakers) {
    std::string out;
    for (const auto& s : segments) {
        const auto it = speakers.find(s.speaker);
        out += "[" + mmss(s.start_ms) + "] " + (it != speakers.end() ? it->second : s.speaker) + ": " + trim(s.text) + "\n";
    }
    return out;
}

json knowledge_turns(const std::vector<Segment>& segments, const std::map<std::string, std::string>& speakers) {
    json a = json::array();
    for (const auto& s : segments) {
        const auto it = speakers.find(s.speaker);
        a.push_back({{"speaker", it != speakers.end() ? it->second : s.speaker}, {"start_ms", s.start_ms}, {"text", s.text}});
    }
    return a;
}

bool supported_language(const std::string& code) {
    static const std::set<std::string> langs = {"cs", "de", "en"};
    return langs.count(to_lower(code)) > 0;
}

bool parakeet_language(const std::string& code) {
    static const std::set<std::string> langs = {"bg", "cs", "da", "de", "el", "en", "es", "et", "fi", "fr", "hr", "hu", "it",
                                                "lt", "lv", "mt", "nl", "pl", "pt", "ro", "ru", "sk", "sl", "sv", "uk"};
    return langs.count(to_lower(code)) > 0;
}

std::string vote_language(const std::vector<std::string>& votes) {
    std::string best;
    std::ptrdiff_t best_n = 0;
    for (size_t i = 0; i < votes.size(); ++i) {
        if (votes[i].empty()) continue;
        const std::ptrdiff_t n = std::count(votes.begin(), votes.end(), votes[i]);
        if (n > best_n) best = votes[i], best_n = n;
    }
    return best;
}

std::vector<std::string> label_spans(const std::vector<std::pair<long long, long long>>& spans,
                                     const std::vector<Turn>& turns, int& next) {
    std::vector<std::string> out;
    if (turns.empty()) {
        if (!spans.empty()) {
            out.assign(spans.size(), "s" + std::to_string(next));
            ++next;
        }
        return out;
    }
    std::map<int, int> number;  // cluster -> speaker number
    for (const auto& [b, e] : spans) {
        int cluster = turns.front().cluster;
        long long best_overlap = 0, best_gap = -1;
        for (const auto& t : turns) {
            const long long overlap = std::min(e, t.end_ms) - std::max(b, t.start_ms);
            if (overlap > best_overlap) {
                best_overlap = overlap;
                cluster = t.cluster;
            } else if (best_overlap == 0) {
                const long long gap = -overlap;
                if (best_gap < 0 || gap < best_gap) {
                    best_gap = gap;
                    cluster = t.cluster;
                }
            }
        }
        auto it = number.find(cluster);
        if (it == number.end()) it = number.emplace(cluster, next++).first;
        out.push_back("s" + std::to_string(it->second));
    }
    return out;
}

// ---- speech ----------------------------------------------------------------------------------------------------------

std::vector<Span> energy_vad(const std::int16_t* pcm, size_t n, std::uint32_t rate, double threshold) {
    std::vector<Span> out;
    if (!pcm || rate == 0) return out;
    const size_t frame = std::max<size_t>(1, rate * 30 / 1000);
    const size_t join_gap = rate * 300 / 1000, min_len = rate * 200 / 1000;
    for (size_t at = 0; at < n; at += frame) {
        const size_t end = std::min(n, at + frame);
        double sum = 0;
        for (size_t i = at; i < end; ++i) sum += static_cast<double>(pcm[i]) * pcm[i];
        const double rms = std::sqrt(sum / static_cast<double>(end - at));
        if (rms < threshold) continue;
        if (!out.empty() && at - out.back().end <= join_gap) {
            Span& s = out.back();
            const double w = static_cast<double>(s.end - s.begin);
            s.rms = (s.rms * w + rms * static_cast<double>(end - at)) / (w + static_cast<double>(end - at));
            s.end = end;
        } else {
            out.push_back({at, end, rms});
        }
    }
    out.erase(std::remove_if(out.begin(), out.end(), [&](const Span& s) { return s.end - s.begin < min_len; }), out.end());
    return out;
}

Result<json> TestEngine::transcribe(const std::vector<TrackAudio>& tracks, const std::string& language) {
    json segs = json::array();
    int n = 0;
    for (const auto& t : tracks) {
        const auto spans = energy_vad(t.pcm.data(), t.pcm.size(), t.rate);
        double lo = 1e18, hi = 0;
        for (const auto& s : spans) {
            lo = std::min(lo, s.rms);
            hi = std::max(hi, s.rms);
        }
        const bool own = t.track == "local";  // a manual microphone capture may hold a whole room
        for (const auto& s : spans) {
            // Two remote speakers by loudness: a stand-in for diarization that fixtures can steer.
            const std::string speaker = own ? "me" : (hi > lo * 1.5 && s.rms > (lo + hi) / 2 ? "s2" : "s1");
            segs.push_back({{"track", t.track},
                            {"start_ms", static_cast<long long>(s.begin) * 1000 / t.rate},
                            {"end_ms", static_cast<long long>(s.end) * 1000 / t.rate},
                            {"speaker", speaker},
                            {"text", "Segment " + std::to_string(++n)}});
        }
    }
    return json{{"language", language.empty() ? "en" : language}, {"segments", segs}, {"engine", "test"}};
}

// ---- Trigger kind meeting --------------------------------------------------------------------------------------------

Status validate_meeting_conditions(const json& c) {
    if (!c.is_object()) return make_error("invalid_trigger", "conditions are an object");
    for (const auto& [k, v] : c.items()) {
        if (k != "event" && k != "app" && k != "title_contains") return make_error("invalid_trigger", "unknown condition \"" + k + "\"");
        if (!v.is_string()) return make_error("invalid_trigger", k + " is text");
    }
    const std::string e = jstr(c, "event", "transcript_ready");
    if (e != "detected" && e != "transcript_ready") return make_error("invalid_trigger", "event is detected or transcript_ready");
    return {};
}

bool meeting_matches(const json& c, const json& m) {
    if (jstr(c, "event", "transcript_ready") != jstr(m, "event")) return false;
    if (const std::string app = jstr(c, "app"); !app.empty() && !iequals(app, jstr(m, "app"))) return false;
    return contains_folded(jstr(m, "title"), jstr(c, "title_contains"));
}

json meeting_trigger_detail(const std::string& event, const std::string& meeting_id, const std::string& thread_id,
                            const std::string& calendar_event_id) {
    json d = {{"source", "meeting"}, {"meeting_event", event}, {"meeting_id", meeting_id}, {"thread_id", thread_id},
              {"note", "The meeting's transcript is in its Thread; it is untrusted content."}};
    if (!calendar_event_id.empty()) d["calendar_event_id"] = calendar_event_id;
    return d;
}

}  // namespace wb::meeting
