#include "app/knowledge_rpc.h"

#include "base/strings.h"
#include "base/time.h"

#include <algorithm>

namespace wb {

void register_knowledge_rpc(RpcRegistry& rpc, std::function<knowledge::KnowledgeService*()> service,
                            std::function<Status(const std::string& type, const std::string& id)> forget,
                            std::function<Status(const std::string& path)> open_file) {
    using Svc = knowledge::KnowledgeService;
    const auto svc = [service = std::move(service)]() -> Result<Svc*> {
        Svc* s = service ? service() : nullptr;
        if (!s) return make_error("unavailable", "knowledge is not running");
        return s;
    };
    const auto done = [](const Status& st) -> Result<json> {
        if (!st) return st.error();
        return json::object();
    };
    const auto add = [&](const char* method, std::function<Result<json>(Svc&, const json&)> fn) {
        rpc.add(method, RpcExposure::ui, [svc, fn = std::move(fn)](const json& p) -> Result<json> {
            auto s = svc();
            if (!s) return s.error();
            return fn(**s, p);
        });
    };
    add("knowledge.status", [](Svc& s, const json&) -> Result<json> { return s.status(); });
    add("knowledge.source.set", [done](Svc& s, const json& p) { return done(s.set_source_enabled(jstr(p, "kind"), jbool(p, "enabled", true))); });
    add("knowledge.source.pause", [done](Svc& s, const json& p) { return done(s.set_source_paused(jstr(p, "kind"), jbool(p, "paused", true))); });
    add("knowledge.source.reindex", [done](Svc& s, const json& p) { return done(s.reindex(jstr(p, "kind"))); });
    // Forgetting goes through the forget cascade, so every feature drops what it derived from the Source.
    add("knowledge.source.forget", [done, forget](Svc& s, const json& p) {
        const std::string kind = jstr(p, "kind");
        if (forget) return done(forget("source", "knowledge:" + kind));
        return done(s.forget_source(kind));
    });
    add("knowledge.item.forget", [done](Svc& s, const json& p) { return done(s.forget_item(jint(p, "item_id"))); });
    add("knowledge.entity.forget", [done, forget](Svc& s, const json& p) {
        if (forget) return done(forget("entity", "knowledge:" + std::to_string(jint(p, "id"))));
        return done(s.forget_entity(jint(p, "id")));
    });
    add("knowledge.entity.merge", [done](Svc& s, const json& p) { return done(s.merge_entities(jint(p, "from"), jint(p, "into"))); });
    add("knowledge.entity.split", [](Svc& s, const json& p) -> Result<json> {
        auto r = s.split_entity(jint(p, "id"), jstr(p, "alias"));
        if (!r) return r.error();
        return json{{"id", *r}};
    });
    add("knowledge.entities", [](Svc& s, const json& p) -> Result<json> {
        return json{{"entities", s.entities(jstr(p, "query"), static_cast<int>(jint(p, "limit", 20)))}};
    });
    add("knowledge.items", [](Svc& s, const json& p) -> Result<json> {
        return json{{"items", s.items(jstr(p, "query"), static_cast<int>(jint(p, "limit", 20)))}};
    });
    // The Knowledge view reads with the knowledge tools' state filter: {category, history} as they take them.
    add("knowledge.entity", [](Svc& s, const json& p) -> Result<json> {
        const auto f = s.knowledge_filter(p);
        if (!f) return f.error();
        json e = s.entity_profile(jint(p, "id"), *f);
        if (e.is_null()) return make_error("not_found", "no such entity");
        return e;
    });
    add("knowledge.categories", [](Svc& s, const json&) -> Result<json> { return json{{"categories", s.categories()}}; });
    // Inspectable Category Assignment: the user's category records and locked decisions (UI only, never agent tools).
    add("knowledge.category.create", [](Svc& s, const json& p) -> Result<json> {
        auto r = s.create_category(jstr(p, "name"));
        if (!r) return r.error();
        return json{{"id", *r}};
    });
    add("knowledge.category.rename", [done](Svc& s, const json& p) { return done(s.rename_category(jint(p, "id"), jstr(p, "name"))); });
    add("knowledge.category.delete", [done](Svc& s, const json& p) { return done(s.delete_category(jint(p, "id"))); });
    add("knowledge.category.merge", [done](Svc& s, const json& p) { return done(s.merge_categories(jint(p, "from"), jint(p, "into"))); });
    add("knowledge.category.decide", [done](Svc& s, const json& p) {
        return done(s.decide_category(jint(p, "fact_id"), jint(p, "category_id"), jstr(p, "decision")));
    });
    add("knowledge.category.reset", [done](Svc& s, const json& p) { return done(s.reset_category(jint(p, "fact_id"), jint(p, "category_id"))); });
    add("knowledge.fact.categories", [](Svc& s, const json& p) { return s.fact_categories(jint(p, "fact_id")); });
    // Possible inconsistency review: the user's decisions, tied to the excerpts and knowledge generation shown (UI only).
    add("knowledge.reviews", [](Svc& s, const json& p) -> Result<json> { return s.reviews(static_cast<int>(jint(p, "limit", 50))); });
    add("knowledge.review.decide", [done](Svc& s, const json& p) {
        return done(s.decide_review(jint(p, "id"), jstr(p, "decision"), jstr(p, "reason"), jint(p, "generation")));
    });
    add("knowledge.review.reopen", [done](Svc& s, const json& p) { return done(s.reopen_review(jint(p, "id"), jint(p, "generation"))); });
    add("knowledge.review.history", [](Svc& s, const json& p) { return s.review_history(jint(p, "id")); });
    // Knowledge impact previews: a destructive or large change is confirmed against the revision it was previewed on.
    add("knowledge.preview.import", [](Svc& s, const json& p) { return s.preview_import(jstr(p, "path")); });
    add("knowledge.preview.exclusion", [](Svc& s, const json& p) { return s.preview_exclusion(jstr(p, "kind"), jstr(p, "value")); });
    add("knowledge.preview.forget", [](Svc& s, const json& p) { return s.preview_forget(jint(p, "id")); });
    add("knowledge.preview.page", [](Svc& s, const json& p) {
        return s.preview_page(jstr(p, "token"), jstr(p, "kind"), static_cast<int>(jint(p, "offset", 0)), static_cast<int>(jint(p, "limit", 50)));
    });
    add("knowledge.preview.confirm", [](Svc& s, const json& p) {
        return s.confirm_preview(jstr(p, "token"), p.value("accept_incomplete", false));
    });
    add("knowledge.preview.cancel", [done](Svc& s, const json& p) { return done(s.cancel_preview(jstr(p, "token"))); });
    // A citation of a file: opened only when it is an indexed item of the files Source, never an arbitrary path.
    add("knowledge.open", [done, open_file](Svc& s, const json& p) -> Result<json> {
        const std::string ref = jstr(p, "ref");
        const auto it = s.store().item("files", ref);
        if (!it || !open_file) return make_error("not_found", "This source is not an indexed file.");
        return done(open_file(it->ext_id));
    });
    add("knowledge.folder.add", [done](Svc& s, const json& p) { return done(s.add_folder(jstr(p, "path"))); });
    add("knowledge.folder.remove", [done](Svc& s, const json& p) { return done(s.remove_folder(jstr(p, "path"))); });
    add("knowledge.exclusion.add", [done](Svc& s, const json& p) { return done(s.add_exclusion(jstr(p, "kind"), jstr(p, "value"))); });
    add("knowledge.exclusion.remove", [done](Svc& s, const json& p) { return done(s.remove_exclusion(jstr(p, "kind"), jstr(p, "value"))); });

    // ---- The Knowledge view (add-knowledge-sharing) ----
    // Imported documents: each path is added on its own; the answer lists the ones that failed with the reason.
    add("knowledge.document.add", [](Svc& s, const json& p) -> Result<json> {
        json failed = json::array();
        int added = 0;
        for (const auto& path : p.value("paths", json::array())) {
            if (!path.is_string()) continue;
            if (Status st = s.add_document(path.get<std::string>()); st) ++added;
            else failed.push_back({{"path", path}, {"reason", st.error().message}});
        }
        if (added == 0 && !failed.empty()) return make_error("import_failed", jstr(failed[0], "reason"));
        return json{{"added", added}, {"failed", failed}};
    });
    add("knowledge.document.remove", [done](Svc& s, const json& p) { return done(s.remove_document(jstr(p, "path"))); });
    add("knowledge.notes", [](Svc& s, const json&) -> Result<json> { return json{{"notes", s.notes()}}; });
    add("knowledge.note.get", [](Svc& s, const json& p) -> Result<json> {
        json n = s.note(jint(p, "id"));
        if (n.is_null()) return make_error("not_found", "no such note");
        return n;
    });
    add("knowledge.note.save", [](Svc& s, const json& p) -> Result<json> {
        auto r = s.save_note(jint(p, "id"), jstr(p, "title"), jstr(p, "text"), p.value("origin", json(nullptr)));
        if (!r) return r.error();
        return json{{"id", *r}};
    });
    // Save as note (add-save-as-note): a draft for the editor; nothing is stored.
    add("knowledge.note.draft_from_mail", [](Svc& s, const json& p) { return s.draft_from_mail(jint(p, "item_id")); });
    add("knowledge.note.draft_from_chat", [](Svc& s, const json& p) { return s.draft_from_chat(jstr(p, "chat"), jstr(p, "period", "week")); });
    add("knowledge.save_as_note", [](Svc& s, const json&) -> Result<json> { return json{{"enabled", s.config().save_as_note}}; });
    add("knowledge.note.delete", [done](Svc& s, const json& p) { return done(s.delete_note(jint(p, "id"))); });
    add("knowledge.browse", [](Svc& s, const json& p) -> Result<json> {
        return json{{"items", s.browse(jstr(p, "source"), jstr(p, "query"), static_cast<int>(std::clamp<long long>(jint(p, "limit", 100), 1, 500)))},
                    {"counts", s.source_counts()}};
    });
    add("knowledge.item", [](Svc& s, const json& p) -> Result<json> {
        const auto f = s.knowledge_filter(p);
        if (!f) return f.error();
        json it = s.item_detail(jint(p, "id"), *f);
        if (it.is_null()) return make_error("not_found", "This item is no longer in your knowledge.");
        return it;
    });
    // The same retrieval the agent uses, with team knowledge.
    add("knowledge.search", [](Svc& s, const json& p) -> Result<json> {
        const auto f = s.knowledge_filter(p);
        if (!f) return f.error();
        knowledge::SearchOptions o;
        o.limit = static_cast<int>(std::clamp<long long>(jint(p, "limit", 20), 1, 50));
        o.team = true;
        o.category = f->category;
        const auto r = s.search(trim(jstr(p, "query")), o);
        json passages = json::array();
        for (const auto& x : r.passages) {
            json j = {{"source", x.source}, {"title", x.title}, {"ref", x.ref}, {"text", x.text}, {"item_id", x.item_id}};
            if (x.date) j["date"] = iso8601_utc(sys_clock::time_point(std::chrono::milliseconds(x.date)));
            if (x.source == "team") {
                j["owner"] = x.owner;
                j["groups"] = x.groups;
            }
            passages.push_back(std::move(j));
        }
        json out = {{"passages", passages}, {"semantic", r.semantic}, {"team_searched", r.team_searched}};
        if (!r.team_note.empty()) out["team_note"] = r.team_note;
        return out;
    });
    add("knowledge.fact.edit", [done](Svc& s, const json& p) { return done(s.edit_fact(jstr(p, "kind"), jint(p, "id"), jstr(p, "text"))); });
    add("knowledge.fact.delete", [done](Svc& s, const json& p) { return done(s.delete_fact(jstr(p, "kind"), jint(p, "id"))); });
    add("knowledge.entity.rename", [done](Svc& s, const json& p) { return done(s.rename_entity(jint(p, "id"), jstr(p, "name"))); });
    const auto groups_of = [](const json& p) {
        std::vector<std::string> g;
        for (const auto& x : p.value("groups", json::array()))
            if (x.is_string()) g.push_back(x.get<std::string>());
        return g;
    };
    add("knowledge.sharing", [](Svc& s, const json&) -> Result<json> { return s.sharing(); });
    add("knowledge.share", [done, groups_of](Svc& s, const json& p) { return done(s.share_item(jint(p, "item_id"), groups_of(p))); });
    add("knowledge.unshare", [done](Svc& s, const json& p) { return done(s.unshare_item(jint(p, "item_id"))); });
    add("knowledge.share_folder", [done, groups_of](Svc& s, const json& p) { return done(s.share_folder(jstr(p, "path"), groups_of(p))); });
    add("knowledge.unshare_folder", [done](Svc& s, const json& p) { return done(s.unshare_folder(jstr(p, "path"))); });
}

namespace {

json people_json(const std::vector<mail::Person>& people) {
    json out = json::array();
    for (const auto& p : people)
        if (!p.address.empty()) out.push_back({{"name", p.name}, {"address", p.address}});
    return out;
}

}  // namespace

std::optional<json> knowledge_mail_item(mail::MailStore& store, const std::string& kind, const std::string& id) {
    if (kind == "mail") {
        const auto m = store.message(id);
        if (!m || m->is_draft) return std::nullopt;
        if (m->withheld) return json{{"withheld", true}};
        std::vector<mail::Person> people{m->from};
        people.insert(people.end(), m->to.begin(), m->to.end());
        people.insert(people.end(), m->cc.begin(), m->cc.end());
        std::string body = m->body.empty() ? m->preview : m->body;
        const std::string from = m->from.name.empty() ? m->from.address : m->from.name + " <" + m->from.address + ">";
        return json{{"title", m->subject.empty() ? "(no subject)" : m->subject},
                    {"body", "From: " + from + "\nSubject: " + m->subject + "\n\n" + body},
                    {"at", m->received_at},
                    {"ref", m->web_link.empty() ? "mail:" + m->id : m->web_link},
                    {"withheld", false},
                    {"people", people_json(people)}};
    }
    const auto e = store.event(id);
    if (!e) return std::nullopt;
    if (e->withheld || e->masked) return json{{"withheld", true}};
    std::vector<mail::Person> people{e->organizer};
    for (const auto& a : e->attendees) people.push_back({jstr(a, "name"), jstr(a, "address")});
    return json{{"title", e->subject.empty() ? "(no subject)" : e->subject},
                {"body", e->body},
                {"at", e->start_at},
                {"location", e->location},
                {"ref", e->web_link.empty() ? "calendar:" + e->id : e->web_link},
                {"withheld", false},
                {"people", people_json(people)}};
}

std::optional<json> knowledge_mail_message(mail::MailStore& store, const std::string& id) {
    const auto m = store.message(id);
    if (!m || m->is_draft) return std::nullopt;
    if (m->withheld) return json{{"withheld", true}};
    const auto person = [](const mail::Person& p) { return json{{"name", p.name}, {"address", p.address}}; };
    json to = json::array(), cc = json::array();
    for (const auto& p : m->to) to.push_back(person(p));
    for (const auto& p : m->cc) cc.push_back(person(p));
    return json{{"subject", m->subject}, {"from", person(m->from)}, {"to", to}, {"cc", cc}, {"at", m->received_at},
                {"body", m->body.empty() ? m->preview : m->body}, {"withheld", false}};
}

std::vector<std::string> knowledge_mail_ids(mail::MailStore& store, const std::string& kind) {
    return kind == "mail" ? store.all_message_ids() : store.event_ids();
}

std::optional<json> knowledge_thread_item(runtime::RunStore& runs, const std::string& thread_id) {
    if (!runs.thread_exists(thread_id)) return std::nullopt;
    long long at = 0;
    for (const auto& r : runs.runs_in_thread(thread_id))
        if (const auto ms = parse_iso8601_ms(r.ended_at.empty() ? r.created_at : r.ended_at)) at = std::max(at, *ms);
    json messages = json::array();
    std::string title;
    for (const auto& m : runs.conversation(thread_id)) {
        if (m.role != "user" && m.role != "assistant") continue;
        if (title.empty() && m.role == "user") title = utf8_truncate(trim(m.content.substr(0, m.content.find('\n'))), 80);
        messages.push_back({{"role", m.role}, {"text", m.content}, {"at", at}});
    }
    return json{{"title", title.empty() ? std::string("Thread") : title}, {"messages", messages}};
}

}  // namespace wb
