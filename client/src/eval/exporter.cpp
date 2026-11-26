#include "eval/exporter.h"

#include "eval/jsonl.h"
#include "eval/registry.h"
#include "eval/wav.h"
#include "mail/mail_store.h"
#include "mail/mail_sync.h"
#include "meeting/audio_file.h"
#include "meeting/meeting_store.h"
#include "storage/database.h"
#include "teams/teams_service.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <system_error>

namespace wb::eval {
namespace {

json person(const mail::Person& p) { return {{"name", p.name}, {"address", p.address}}; }

json people(const std::vector<mail::Person>& list) {
    json out = json::array();
    for (const auto& p : list) out.push_back(person(p));
    return out;
}

std::string folder_stamp(long long ms) {
    const std::time_t t = static_cast<std::time_t>(ms / 1000);
    std::tm tm{};
    gmtime_s(&tm, &t);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y%m%d-%H%M%S", &tm);
    return buf;
}

Error cancelled() { return make_error("cancelled", "The export was cancelled."); }

Error unavailable(const std::string& source) {
    return make_error("unavailable", "The " + source + " Source is not running, so it cannot be exported.");
}

}  // namespace

Status check_request(const ExportRequest& r) {
    if (r.out_dir.empty() || !r.out_dir.is_absolute()) return make_error("invalid_params", "out_dir must be an absolute folder.");
    if (r.from_ms >= r.to_ms) return make_error("invalid_params", "The window must start before it ends.");
    if (r.sources.empty()) return make_error("invalid_params", "Choose at least one Source.");
    for (const auto& s : r.sources)
        if (s != "mail" && s != "teams" && s != "calendar" && s != "meetings")
            return make_error("invalid_params", "Unknown Source " + s + "; use mail, teams, calendar or meetings.");
    return {};
}

Result<std::filesystem::path> Exporter::begin(const ExportRequest& r) {
    if (Status s = check_request(r); !s) return s.error();
    std::error_code ec;
    std::filesystem::create_directories(r.out_dir, ec);
    if (ec) return make_error("write_failed", "Cannot create the export folder: " + ec.message());
    const auto folder = r.out_dir / folder_stamp(env_.now ? env_.now() : 0);
    if (std::filesystem::exists(folder, ec)) return make_error("exists", "The export folder already exists.");
    if (!std::filesystem::create_directory(folder, ec) || ec) return make_error("write_failed", "Cannot create the export folder.");
    return folder;
}

json Exporter::triage_of(const std::string& scope, const std::string& item_id) const {
    if (!env_.app_db) return nullptr;
    const std::lock_guard lock(env_.app_db->mutex());
    auto q = env_.app_db->prepare("SELECT state, outcome, reason, action, category, suggestion, updated_at, instruction_id "
                                  "FROM triage_items WHERE scope=?1 AND item_id=?2");
    q.bind(1, scope).bind(2, item_id);
    if (!q.step()) return nullptr;
    return {{"state", q.col_text(0)}, {"outcome", q.col_text(1)}, {"reason", q.col_text(2)}, {"action", q.col_text(3)},
            {"category", q.col_text(4)}, {"suggestion", q.col_text(5)}, {"updated_at", q.col_int(6)},
            {"instruction_id", q.col_text(7)}};
}

json Exporter::agenda_of(const std::string& kind, const std::string& source_id) const {
    json out = json::array();
    if (!env_.app_db) return out;
    const std::lock_guard lock(env_.app_db->mutex());
    auto q = env_.app_db->prepare(
        "SELECT i.id, i.title, i.action, i.due_at, i.state, i.user_edited, i.state_at, i.instruction_id, s.quote "
        "FROM agenda_sources s JOIN agenda_items i ON i.id=s.item_id WHERE s.kind=?1 AND s.source_id=?2");
    q.bind(1, kind).bind(2, source_id);
    while (q.step())
        out.push_back({{"id", q.col_text(0)}, {"title", q.col_text(1)}, {"action", q.col_text(2)}, {"due_at", q.col_int(3)},
                       {"state", q.col_text(4)}, {"user_edited", q.col_int(5) != 0}, {"state_at", q.col_int(6)},
                       {"instruction_id", q.col_text(7)}, {"quote", q.col_text(8)}});
    return out;
}

json Exporter::decisions_of(const std::string& item_ref) const {
    json out = json::array();
    if (!env_.decisions) return out;
    for (const auto& d : env_.decisions->for_item(item_ref))
        out.push_back({{"at", d.at_ms}, {"step", d.step}, {"kind", d.kind}, {"result", d.result_ref}, {"before", d.before},
                       {"after", d.after}, {"instruction_id", d.instruction_id}});
    return out;
}

Result<ExportCounts> Exporter::export_mail(const std::filesystem::path& folder, const ExportRequest& r, const CancelToken& cancel) {
    ExportCounts counts;
    mail::MailStore* store = env_.mail ? env_.mail() : nullptr;
    if (!store) return unavailable("mail");
    auto out = JsonlWriter::open(folder / "mail.jsonl");
    if (!out) return out.error();
    // Own folders (sent, drafts, outbox): their mail is the user's; a later one in the conversation is a reply.
    std::set<std::string> own;
    std::map<std::string, std::string> names;
    for (const auto& f : store->folders()) {
        names[f.id] = f.well_known.empty() ? f.name : f.well_known;
        if (mail::own_mail_folder(f)) own.insert(f.id);
    }
    std::map<std::string, std::vector<long long>> sent_at;  // conversation -> times the user sent in it
    for (const auto& id : store->all_message_ids())
        if (const auto m = store->message(id); m && own.count(m->folder_id) && !m->is_draft && !m->conversation_id.empty())
            sent_at[m->conversation_id].push_back(m->received_at);
    for (const auto& m : store->messages_between(r.from_ms, r.to_ms)) {
        if (cancel.cancelled()) return cancelled();
        if (m.withheld) {
            ++counts.withheld;
            continue;
        }
        bool replied = false;
        if (const auto it = sent_at.find(m.conversation_id); it != sent_at.end() && !own.count(m.folder_id))
            for (const long long t : it->second) replied = replied || t > m.received_at;
        const std::string ref = item_ref_mail(m.id);
        json legacy = nullptr;
        json triage = triage_of("mail", m.id);
        if (triage.is_object() && !jstr(triage, "suggestion").empty())
            legacy = {{"suggestion", triage["suggestion"]}, {"at", triage["updated_at"]}, {"at_approximate", true}};
        json rec = {{"id", ref},
                    {"item",
                     {{"from", person(m.from)}, {"to", people(m.to)}, {"cc", people(m.cc)}, {"received_at", m.received_at},
                      {"subject", m.subject}, {"folder", names.count(m.folder_id) ? names[m.folder_id] : std::string{}},
                      {"conversation_id", m.conversation_id}, {"body", m.body}, {"categories", m.categories},
                      {"sent_by_user", own.count(m.folder_id) > 0}, {"user_replied", replied}, {"user_replied_approximate", true}}},
                    {"skowork",
                     {{"triage", triage}, {"agenda", agenda_of("mail", m.id)},
                      {"facts", env_.facts_of ? env_.facts_of("mail", m.id) : json::array()}}},
                    {"user", {{"decisions", decisions_of(ref)}, {"legacy", legacy}}}};
        if (Status s = out->write(rec); !s) return s.error();
        ++counts.written;
    }
    if (Status s = out->close(); !s) return s.error();
    return counts;
}

Result<ExportCounts> Exporter::export_teams(const std::filesystem::path& folder, const ExportRequest& r, const CancelToken& cancel) {
    ExportCounts counts;
    teams::TeamsService* teams = env_.teams ? env_.teams() : nullptr;
    if (!teams) return unavailable("teams");
    auto out = JsonlWriter::open(folder / "teams.jsonl");
    if (!out) return out.error();
    for (const auto& cid : teams->conversation_ids()) {
        if (cancel.cancelled()) return cancelled();
        const auto conv = teams->conversation(cid);
        for (const auto& m : teams->messages(cid, r.from_ms, r.to_ms, 1'000'000)) {
            if (m.system) continue;
            const std::string ref = item_ref_teams(cid, m.id);
            const std::string triage_id = cid + "|" + m.id;
            json triage = triage_of("teams", triage_id);
            json legacy = nullptr;
            if (triage.is_object() && !jstr(triage, "suggestion").empty())
                legacy = {{"suggestion", triage["suggestion"]}, {"at", triage["updated_at"]}, {"at_approximate", true}};
            json rec = {{"id", ref},
                        {"item",
                         {{"conversation_id", cid}, {"conversation_type", conv ? conv->type : std::string{}},
                          {"topic", conv ? conv->topic : std::string{}}, {"parent_id", m.parent_id},
                          {"sender", {{"name", m.sender_name}, {"email", m.sender_email}}}, {"at", m.at_ms},
                          {"text", m.text}, {"mine", m.mine}, {"mentions_me", m.mentions_me}}},
                        {"skowork", {{"triage", triage}, {"agenda", agenda_of("teams", m.id)}, {"facts", json::array()}}},
                        {"user", {{"decisions", decisions_of(ref)}, {"legacy", legacy}}}};
            if (Status s = out->write(rec); !s) return s.error();
            ++counts.written;
        }
    }
    if (Status s = out->close(); !s) return s.error();
    return counts;
}

Result<ExportCounts> Exporter::export_calendar(const std::filesystem::path& folder, const ExportRequest& r, const CancelToken& cancel) {
    ExportCounts counts;
    mail::MailStore* store = env_.mail ? env_.mail() : nullptr;
    if (!store) return unavailable("calendar");
    auto out = JsonlWriter::open(folder / "calendar.jsonl");
    if (!out) return out.error();
    for (const auto& e : store->events_between(r.from_ms, r.to_ms)) {
        if (cancel.cancelled()) return cancelled();
        if (e.withheld) {
            ++counts.withheld;
            continue;
        }
        const std::string ref = item_ref_calendar(e.id);
        json rec = {{"id", ref},
                    {"item",
                     {{"subject", e.masked ? std::string{} : e.subject}, {"body", e.masked ? std::string{} : e.body},
                      {"location", e.location}, {"start_at", e.start_at}, {"end_at", e.end_at}, {"all_day", e.all_day},
                      {"organizer", person(e.organizer)}, {"attendees", e.attendees}, {"is_organizer", e.is_organizer},
                      {"response", e.response}, {"series_master_id", e.series_master_id}, {"cancelled", e.cancelled},
                      {"masked", e.masked}, {"online", !e.online_url.empty()}}},
                    {"skowork", {{"triage", triage_of("calendar", e.id)}, {"agenda", agenda_of("calendar", e.id)}}},
                    {"user", {{"decisions", decisions_of(ref)}}}};
        if (Status s = out->write(rec); !s) return s.error();
        ++counts.written;
    }
    if (Status s = out->close(); !s) return s.error();
    return counts;
}

namespace {

std::vector<meeting::MeetingRecord> meetings_in(meeting::MeetingStore& store, const ExportRequest& r) {
    std::vector<meeting::MeetingRecord> out;
    for (auto& m : store.list(100'000)) {
        const long long at = m.started_at ? m.started_at : m.detected_at;
        if (at >= r.from_ms && at < r.to_ms) out.push_back(std::move(m));
    }
    return out;
}

std::vector<std::filesystem::path> tracks_of(const meeting::MeetingRecord& m) {
    std::vector<std::filesystem::path> out;
    if (m.audio_deleted || m.audio_dir.empty()) return out;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(meeting::path_from_utf8(m.audio_dir), ec))
        if (e.path().extension() == ".skau") out.push_back(e.path());
    return out;
}

}  // namespace

long long Exporter::audio_size(const ExportRequest& r) const {
    meeting::MeetingStore* meetings = env_.meetings ? env_.meetings() : nullptr;
    if (!r.audio || !meetings || !r.sources.count("meetings")) return 0;
    long long total = 0;
    std::error_code ec;
    for (const auto& m : meetings_in(*meetings, r))
        for (const auto& t : tracks_of(m)) {
            const auto size = std::filesystem::file_size(t, ec);
            if (!ec) total += static_cast<long long>(size) + 44;  // 16-bit PCM in either container, plus the WAV header
        }
    return total;
}

Result<ExportCounts> Exporter::export_meetings(const std::filesystem::path& folder, const ExportRequest& r, const CancelToken& cancel) {
    ExportCounts counts;
    meeting::MeetingStore* meetings = env_.meetings ? env_.meetings() : nullptr;
    if (!meetings) return unavailable("meetings");
    mail::MailStore* store = env_.mail ? env_.mail() : nullptr;
    auto out = JsonlWriter::open(folder / "meetings.jsonl");
    if (!out) return out.error();
    for (const auto& m : meetings_in(*meetings, r)) {
        if (cancel.cancelled()) return cancelled();
        json speakers = json::object(), meta = json::object();
        for (const auto& [k, v] : m.speakers) {
            speakers[k] = v;
            const auto it = m.speaker_meta.find(k);
            const meeting::SpeakerMeta sm = it == m.speaker_meta.end() ? meeting::SpeakerMeta{} : it->second;
            meta[k] = {{"by", sm.by}, {"at", sm.at_ms}};
        }
        json event = nullptr;
        if (store && !m.event_id.empty())
            if (const auto e = store->event(m.event_id); e && !e->withheld && !e->masked)
                event = {{"id", e->id}, {"subject", e->subject}, {"body", e->body}, {"organizer", person(e->organizer)},
                         {"attendees", e->attendees}, {"start_at", e->start_at}, {"end_at", e->end_at}};
        json audio = "unavailable";
        const auto tracks = tracks_of(m);
        if (r.audio && !tracks.empty()) {
            audio = json::object();
            const auto dir = folder / "audio" / m.id;
            std::error_code ec;
            std::filesystem::create_directories(dir, ec);
            if (ec) return make_error("write_failed", "Cannot create the audio folder.");
            for (const auto& t : tracks) {
                auto decoded = meeting::read_track(t);
                if (!decoded) continue;
                const std::string name = t.stem().string() + ".wav";
                if (Status s = write_wav(dir / name, decoded->pcm, static_cast<int>(decoded->header.sample_rate)); !s) return s.error();
                audio[t.stem().string()] = "audio/" + m.id + "/" + name;
            }
        } else if (!tracks.empty()) {
            audio = "not_requested";
        }
        json rec = {{"id", item_ref_meeting(m.id)},
                    {"item",
                     {{"title", m.title}, {"state", m.state}, {"source", m.source}, {"app", m.app},
                      {"started_at", m.started_at}, {"ended_at", m.ended_at}, {"language", m.language},
                      {"attendees", m.attendees}, {"vocabulary", m.vocabulary}, {"segments", m.segments},
                      {"speakers", speakers}, {"speaker_meta", meta}, {"event", event}, {"audio", audio}}},
                    {"skowork", {{"agenda", agenda_of("meeting", m.id)},
                                 {"facts", env_.facts_of ? env_.facts_of("meetings", m.id) : json::array()}}},
                    {"user", {{"decisions", decisions_of(item_ref_meeting(m.id))}}}};
        if (Status s = out->write(rec); !s) return s.error();
        ++counts.written;
    }
    if (Status s = out->close(); !s) return s.error();
    return counts;
}

Status Exporter::write_manifest(const std::filesystem::path& folder, const ExportRequest& r,
                                const std::map<std::string, ExportCounts>& counts, bool complete) const {
    json c = json::object();
    for (const auto& [source, n] : counts) c[source] = {{"written", n.written}, {"withheld", n.withheld}};
    json ids = json::object();
    for (Step s : {Step::triage, Step::agenda, Step::knowledge, Step::diarize, Step::asr})
        ids[std::string(to_string(s))] = Variant::defaults(s).instruction_id();
    json sources = json::array();
    for (const auto& s : r.sources) sources.push_back(s);
    const json manifest = {{"window", {{"from", r.from_ms}, {"to", r.to_ms}}},
                           {"sources", sources},
                           {"audio", r.audio},
                           {"client_version", env_.client_version},
                           {"counts", c},
                           {"instruction_ids", ids},
                           {"complete", complete}};
    auto out = JsonlWriter::open(folder / "manifest.json");
    if (!out) return out.error();
    if (Status s = out->write(manifest); !s) return s;
    return out->close();
}

}  // namespace wb::eval
