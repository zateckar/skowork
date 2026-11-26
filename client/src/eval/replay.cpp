#include "eval/replay.h"
#include "meeting/audio_file.h"

#include "base/strings.h"
#include "eval/jsonl.h"
#include "knowledge/core.h"
#include "mail/types.h"
#include "meeting/core.h"
#include "platform/crypto.h"
#include "triage/classifier.h"
#include "triage/triage.h"

#include <fstream>
#include <set>

namespace wb::eval {
namespace {

std::vector<json> read_jsonl(const std::filesystem::path& file) {
    std::vector<json> out;
    std::ifstream in(file, std::ios::binary);
    std::string line;
    while (std::getline(in, line))
        if (auto j = parse_json(line); j && j->is_object()) out.push_back(std::move(*j));
    return out;
}

// The items a text step replays: mail and Teams messages (and, for triage, calendar events), in file order.
std::vector<json> text_items(const std::filesystem::path& folder, Step step, int max_items) {
    std::vector<json> out;
    std::vector<std::string> files = {"mail.jsonl", "teams.jsonl"};
    if (step == Step::triage) files.push_back("calendar.jsonl");
    for (const auto& f : files)
        for (auto& rec : read_jsonl(folder / f)) {
            if (static_cast<int>(out.size()) >= max_items) return out;
            rec["_kind"] = f.substr(0, f.find('.'));
            out.push_back(std::move(rec));
        }
    return out;
}

std::string failure_kind(const Error& e) {
    if (e.code == "unavailable") return "unavailable";
    if (e.message.find("cut off") != std::string::npos) return "cut_off";
    if (e.message.find("match") != std::string::npos) return "mismatch";
    return "other";
}

mail::Person person_of(const json& j) { return {jstr(j, "name"), jstr(j, "address")}; }

std::string event_text(const json& rec, size_t limit) {
    const std::string kind = jstr(rec, "_kind");
    const json& it = rec["item"];
    if (kind == "teams")
        return triage::teams_event_text(jstr(it["sender"], "name"), jstr(it, "topic"), jint(it, "at"), jstr(it, "text"), limit);
    if (kind == "calendar")
        return triage::calendar_event_text(jstr(it, "subject"), person_of(it["organizer"]), jint(it, "start_at"), jint(it, "end_at"),
                                           jstr(it, "location"), jbool(it, "cancelled"), jstr(it, "body"), limit);
    return triage::mail_event_text(person_of(it["from"]), jstr(it, "subject"), jint(it, "received_at"), jstr(it, "body"), limit);
}

int recipients_of(const json& it) {
    const json* to = jobj(it, "to");
    const json* cc = jobj(it, "cc");
    return (to && to->is_array() ? static_cast<int>(to->size()) : 0) + (cc && cc->is_array() ? static_cast<int>(cc->size()) : 0);
}

}  // namespace

Result<ReplayRequest> replay_request_from_json(const json& params) {
    ReplayRequest r;
    const std::string folder = jstr(params, "folder");
    if (folder.empty()) return make_error("invalid_params", "folder is the export folder to replay.");
    r.folder = meeting::path_from_utf8(folder);
    if (!std::filesystem::exists(r.folder / "manifest.json")) return make_error("not_found", "No export manifest in that folder.");
    const auto step = parse_step(jstr(params, "step"));
    if (!step) return make_error("invalid_params", "step is triage, agenda, knowledge, diarize or asr.");
    r.step = *step;
    if (const auto it = params.find("variant"); it != params.end() && !it->is_null()) r.variant = *it;
    if (const auto it = params.find("max_items"); it != params.end()) {
        if (!it->is_number_integer()) return make_error("invalid_params", "max_items is a whole number from 1 to 2000.");
        r.max_items = it->get<int>();
    }
    if (r.max_items < 1 || r.max_items > kMaxReplayItems) return make_error("invalid_params", "max_items is a whole number from 1 to 2000.");
    if (auto v = Variant::parse(r.step, r.variant); !v) return v.error();
    return r;
}

int cluster_count(const std::string& clusters, const std::vector<std::string>& attendees, const std::string& user_name) {
    if (clusters != "attendees") return -1;
    int others = 0;
    for (const auto& a : attendees)
        if (to_lower(a) != to_lower(user_name)) ++others;
    return others >= 2 ? others : -1;
}

Replayer::~Replayer() {
    cancel();
    if (worker_.joinable()) worker_.join();
}

Result<std::string> Replayer::start(const ReplayRequest& r) {
    auto v = Variant::parse(r.step, r.variant);
    if (!v) return v.error();
    if (r.max_items < 1 || r.max_items > kMaxReplayItems) return make_error("invalid_params", "max_items is a whole number from 1 to 2000.");
    const std::lock_guard lock(mx_);
    if (running_) return make_error("busy", "Another replay is running.");
    if (worker_.joinable()) worker_.join();
    const std::string id = "run_" + hex_encode(random_bytes(6));
    running_ = true;
    cancel_ = false;
    status_ = {{"run", id}, {"state", "running"}, {"step", std::string(to_string(r.step))}, {"done", 0}, {"total", 0},
               {"failures", json::object()}};
    worker_ = std::thread([this, r, variant = *v, id] { run(r, variant, id); });
    return id;
}

json Replayer::status() const {
    const std::lock_guard lock(mx_);
    return status_;
}

void Replayer::cancel() { cancel_ = true; }

void Replayer::wait() {
    if (worker_.joinable()) worker_.join();
}

void Replayer::set(const std::function<void(json&)>& f) {
    const std::lock_guard lock(mx_);
    f(status_);
}

void Replayer::run(ReplayRequest r, Variant v, std::string id) {
    const auto dir = r.folder / "runs" / id;
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    auto out = JsonlWriter::open(dir / "results.jsonl");
    const long long started = env_.now ? env_.now() : 0;
    json failures = json::object();
    long long done = 0, tokens_in = 0, tokens_out = 0;
    std::string error;
    const auto fail_item = [&](const std::string& item, const std::string& kind, const std::string& detail) {
        failures[kind] = failures.value(kind, 0) + 1;
        if (out) (void)out->write({{"id", item}, {"error", {{"kind", kind}, {"detail", detail}}}});
    };
    const auto usage = [&](const model::ChatResponse& resp) {
        tokens_in += resp.usage.prompt;
        tokens_out += resp.usage.completion;
    };
    const auto progress = [&](long long total) {
        set([&](json& s) {
            s["done"] = done;
            s["total"] = total;
            s["failures"] = failures;
        });
    };

    if (!out) {
        error = out.error().message;
    } else if (r.step == Step::triage || r.step == Step::agenda || r.step == Step::knowledge) {
        const auto items = text_items(r.folder, r.step, r.max_items);
        progress(static_cast<long long>(items.size()));
        if (!env_.model) error = "The model is not available.";
        const size_t batch = r.step == Step::triage    ? static_cast<size_t>(v.number("triage.batch_size"))
                             : r.step == Step::agenda ? agenda::kMaxItemsPerBatch
                                                      : 1;
        for (size_t i = 0; error.empty() && i < items.size() && !cancel_; i += batch) {
            const size_t end = std::min(items.size(), i + batch);
            if (r.step == Step::triage) {
                std::vector<triage::Event> events;
                for (size_t k = i; k < end; ++k)
                    events.push_back({jstr(items[k], "id"), "r1", event_text(items[k], static_cast<size_t>(v.number("triage.body_limit")))});
                // Replay usage is attributed to evaluation, not to live triage (review minor 14).
                triage::ClassifyOptions options;
                options.instructions = v.text("triage.instructions");
                options.feature = "eval.triage";
                model::Usage used;
                options.usage = &used;
                auto res = triage::classify(*env_.model, env_.responsibilities ? env_.responsibilities() : std::string{}, events,
                                            options);
                tokens_in += used.prompt;
                tokens_out += used.completion;
                if (!res) {
                    for (const auto& e : events) fail_item(e.id, failure_kind(res.error()), res.error().message);
                } else {
                    // An item the answer leaves out is a failure, never silently missing (review minor 14).
                    std::set<std::string> answered;
                    for (const auto& c : *res) answered.insert(c.id);
                    for (const auto& e : events)
                        if (!answered.count(e.id)) fail_item(e.id, "mismatch", "the answer left this item out");
                    for (const auto& c : *res)
                        (void)out->write({{"id", c.id},
                                          {"result", {{"outcome", c.outcome}, {"reason", c.reason}, {"action", c.action}, {"category", c.category}}}});
                }
            } else if (r.step == Step::agenda) {
                std::vector<agenda::SourceItem> src;
                const agenda::Identity me = env_.identity ? env_.identity() : agenda::Identity{};
                for (size_t k = i; k < end; ++k) {
                    const json& it = items[k]["item"];
                    const bool teams = jstr(items[k], "_kind") == "teams";
                    agenda::SourceItem s;
                    s.kind = teams ? "teams" : "mail";
                    s.id = jstr(items[k], "id");
                    s.conversation = jstr(it, "conversation_id");
                    s.title = teams ? jstr(it, "topic") : jstr(it, "subject");
                    s.text = utf8_truncate(teams ? jstr(it, "text") : jstr(it, "body"), agenda::kMaxItemText);
                    s.author = teams ? jstr(it["sender"], "name") : jstr(it["from"], "name");
                    s.author_is_user = teams ? jbool(it, "mine") : jbool(it, "sent_by_user");
                    s.recipients = recipients_of(it);
                    s.user_addressed = teams ? jbool(it, "mentions_me") : false;
                    if (!teams)
                        for (const auto& p : it.value("to", json::array()))
                            for (const auto& e : me.emails) s.user_addressed = s.user_addressed || to_lower(jstr(p, "address")) == to_lower(e);
                    s.at_ms = teams ? jint(it, "at") : jint(it, "received_at");
                    src.push_back(std::move(s));
                }
                const long long now = env_.now ? env_.now() : 0;
                model::ChatRequest req;
                req.role = model::Role::small;
                req.stream = false;
                req.single_attempt = true;
                req.validate_tool_calls = false;
                req.temperature = 0.0;
                req.max_tokens = 4096;
                req.feature = "eval.agenda";
                req.messages = {{"system", v.text("agenda.instructions"), {}, {}, {}},
                                {"user", agenda::detection_data(src, me, now, env_.tz_offset_min ? env_.tz_offset_min(now) : 0).dump(), {}, {}, {}}};
                auto resp = env_.model->chat(req, {});
                if (!resp || resp->finish_reason != "stop") {
                    for (const auto& s : src) fail_item(s.id, !resp ? "unavailable" : resp->finish_reason == "length" ? "cut_off" : "other", "");
                } else {
                    usage(resp.value());
                    auto cands = agenda::parse_detection(resp->text, src);
                    if (!cands) {
                        for (const auto& s : src) fail_item(s.id, "mismatch", cands.error().message);
                    } else {
                        std::map<std::string, json> by;
                        for (const auto& s : src) by[s.id] = json::array();
                        for (const auto& c : *cands)
                            by[c.source_id].push_back({{"title", c.title}, {"action", c.action}, {"due", c.due}, {"responsibility", c.responsibility},
                                                       {"quote", c.quote}, {"confidence", c.confidence}});
                        for (const auto& [item, list] : by) (void)out->write({{"id", item}, {"result", {{"candidates", list}}}});
                    }
                }
            } else {
                const json& rec = items[i];
                const json& it = rec["item"];
                const bool teams = jstr(rec, "_kind") == "teams";
                const std::string text = teams ? jstr(it, "text") : jstr(it, "body");
                auto chunks = teams ? knowledge::chunk_document(text) : knowledge::chunk_mail(text);
                if (chunks.size() > 8) chunks.resize(8);
                knowledge::SourceContext ctx;
                ctx.source = teams ? "teams" : "mail";
                ctx.title = teams ? jstr(it, "topic") : jstr(it, "subject");
                ctx.author = teams ? jstr(it["sender"], "name") : jstr(it["from"], "name");
                ctx.author_is_user = teams ? jbool(it, "mine") : jbool(it, "sent_by_user");
                ctx.recipients = recipients_of(it);
                ctx.breadth = knowledge::addressing_breadth(ctx.source, ctx.recipients);
                std::string user;
                for (size_t k = 0; k < chunks.size(); ++k) user += knowledge::render_passage(static_cast<int>(k), {}, ctx, chunks[k].text) + "\n\n";
                if (chunks.empty()) {
                    (void)out->write({{"id", jstr(rec, "id")}, {"result", {{"relations", json::array()}}}});
                } else {
                    model::ChatRequest req;
                    req.role = model::Role::small;
                    req.temperature = 0.0;
                    req.max_tokens = knowledge::kKnowledgeOutputTokens;
                    req.feature = "eval.knowledge";
                    req.validate_tool_calls = false;
                    req.messages = {{"system", v.text("knowledge.extraction"), {}, {}, {}}, {"user", user, {}, {}, {}}};
                    auto resp = env_.model->chat(req, {});
                    if (!resp || resp->finish_reason != "stop") {
                        fail_item(jstr(rec, "id"), !resp ? "unavailable" : resp->finish_reason == "length" ? "cut_off" : "other", "");
                    } else {
                        usage(resp.value());
                        const auto ex = knowledge::parse_extraction(resp->text, static_cast<int>(chunks.size()));
                        if (!ex) {
                            fail_item(jstr(rec, "id"), "mismatch", "the answer did not match the schema");
                        } else {
                            json rels = json::array();
                            for (const auto& rel : ex->relations)
                                rels.push_back({{"subject", rel.subject}, {"predicate", rel.predicate}, {"object", rel.object},
                                                {"salience", rel.salience}, {"durability", rel.durability}, {"statement", rel.statement}});
                            (void)out->write({{"id", jstr(rec, "id")}, {"result", {{"relations", rels}}}});
                        }
                    }
                }
            }
            done = static_cast<long long>(end);
            progress(static_cast<long long>(items.size()));
        }
    } else {
        // Speech steps: each meeting with exported audio, one at a time, paused while a capture or live transcription runs.
        auto meetings = read_jsonl(r.folder / "meetings.jsonl");
        if (static_cast<int>(meetings.size()) > r.max_items) meetings.resize(static_cast<size_t>(r.max_items));
        progress(static_cast<long long>(meetings.size()));
        if (!env_.speech) error = "Speech is not available.";
        for (const auto& rec : meetings) {
            if (!error.empty() || cancel_) break;
            const json& it = rec["item"];
            const json audio = it.value("audio", json());
            if (!audio.is_object() || audio.empty()) {
                fail_item(jstr(rec, "id"), "no_audio", "The meeting was exported without audio.");
                ++done;
                progress(static_cast<long long>(meetings.size()));
                continue;
            }
            while (env_.speech_busy && env_.speech_busy() && !cancel_) {
                set([](json& s) { s["state"] = "paused"; });
                for (auto waited = std::chrono::milliseconds(0); waited < env_.pause_poll && !cancel_; waited += std::chrono::milliseconds(50))
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            if (cancel_) break;
            set([](json& s) { s["state"] = "running"; });
            json tracks = json::array();
            // One part per track kind: later parts of a continued capture (remote.2) have no start time in a WAV.
            for (const auto& [k, p] : audio.items())
                if (p.is_string() && k.find('.') == std::string::npos) tracks.push_back(meeting::path_to_utf8(r.folder / meeting::path_from_utf8(p.get<std::string>())));
            std::vector<std::string> attendees;
            for (const auto& a : it.value("attendees", json::array()))
                if (a.is_string()) attendees.push_back(a.get<std::string>());
            const std::string user = env_.user_name ? env_.user_name() : std::string{};
            json settings = {{"vad_threshold", v.number("asr.vad_threshold")},
                             {"hotwords_score", v.number("asr.hotword_score")},
                             {"max_active_paths", static_cast<int>(v.number("asr.beam_paths"))},
                             {"threshold", v.number("diarize.threshold")},
                             {"num_clusters", cluster_count(v.choice("diarize.clusters"), attendees, user)}};
            json req = {{"kind", r.step == Step::asr ? "transcribe" : "diarize"},
                        {"diarize", r.step == Step::diarize},  // a transcription replay does not diarize (review minor 14)
                        {"tracks", tracks},
                        {"language", jstr(it, "language")},
                        {"vocabulary", it.value("vocabulary", json::array())},
                        {"settings", settings}};
            auto resp = env_.speech(req);
            if (!resp) {
                fail_item(jstr(rec, "id"), resp.error().code, resp.error().message);
            } else {
                json result = *resp;
                if (auto segs = meeting::parse_segments(*resp)) {
                    json names = json::object();
                    for (const auto& [k, n] : meeting::name_speakers(*segs, user, attendees)) names[k] = n;
                    result["speakers"] = names;
                }
                (void)out->write({{"id", jstr(rec, "id")}, {"result", result}});
            }
            ++done;
            progress(static_cast<long long>(meetings.size()));
        }
    }
    if (out) (void)out->close();
    const std::string state = !error.empty() ? "failed" : cancel_ ? "cancelled" : "done";
    const json run = {{"run", id},
                      {"step", std::string(to_string(r.step))},
                      {"variant", r.variant},
                      {"instruction_id", v.instruction_id()},
                      {"started", started},
                      {"finished", env_.now ? env_.now() : 0},
                      {"items", done},
                      {"failures_by_kind", failures},
                      {"tokens_in", tokens_in},
                      {"tokens_out", tokens_out},
                      {"state", state},
                      {"error", error}};
    if (auto f = JsonlWriter::open(dir / "run.json")) {
        (void)f->write(run);
        (void)f->close();
    }
    const std::lock_guard lock(mx_);
    status_["state"] = state;
    status_["done"] = done;
    status_["failures"] = failures;
    if (!error.empty()) status_["error"] = error;
    running_ = false;
}

}  // namespace wb::eval
