// Category publications of client/knowledge-graph (Category Publication Eligibility, Category Publication Preview;
// improve-skowork-and-knowledge-control design D8, task 10.2): which facts of chosen categories may be published,
// the exact payload control-plane/team-knowledge receives, and its upload after confirmation.
#include "knowledge/knowledge_service.h"

#include "base/encoding.h"
#include "base/strings.h"
#include "platform/crypto.h"

#include <algorithm>
#include <map>
#include <set>

namespace wb::knowledge {

namespace {

constexpr int kPublicationVersion = 1;            // the mutation schema version this client writes
constexpr size_t kMaxPublicationEntries = 500;     // the server's bound per publication
constexpr size_t kMaxPublicationGroups = 100;
constexpr size_t kMaxPublicLabel = 300;            // titles, in bytes (the server counts UTF-16 units; bytes are stricter)
constexpr int kMaxCategoryFacts = 20'000;          // facts read per category

std::string sha256_hex(std::string_view s) { return hex_encode(sha256(s)); }

std::vector<std::string> strings_of(const json& j) {
    std::vector<std::string> out;
    if (j.is_array())
        for (const auto& v : j)
            if (v.is_string() && !trim(v.get<std::string>()).empty()) out.push_back(trim(v.get<std::string>()));
    return out;
}

// A public label: one line, trimmed, at most kMaxPublicLabel bytes; empty when nothing is left.
std::string public_label(std::string s) {
    for (char& c : s)
        if (c == '\r' || c == '\n' || c == '\t') c = ' ';
    s = trim(s);
    if (s.size() > kMaxPublicLabel) s = trim(utf8_truncate(s, kMaxPublicLabel));
    return s;
}

// The title recipients see for an excerpt's item: never a local path. A file is its name alone.
std::string public_title(const SupportEvidence& e) {
    std::string t = e.title;
    if (e.source == "files") {
        const size_t slash = e.ext_id.find_last_of("/\\");
        t = slash == std::string::npos ? e.ext_id : e.ext_id.substr(slash + 1);
    }
    t = public_label(t);
    return t.empty() ? "Untitled" : t;
}

// The published statement of a fact.
std::string statement(const KnowledgeFactRow& f) {
    std::string p = f.predicate;
    std::replace(p.begin(), p.end(), '_', ' ');
    std::string s = f.subject_name + " " + p + " " + f.object_name;
    if (!f.valid_from.empty()) s += " from " + f.valid_from;
    if (!f.valid_to.empty()) s += " until " + f.valid_to;
    if (!f.scope.empty()) s += " (" + f.scope + ")";
    return s;
}

// A note item's id is the note's id.
long long note_id(const std::string& ext_id) {
    long long n = 0;
    for (char c : ext_id) {
        if (c < '0' || c > '9' || n > 1'000'000'000'000'000LL) return 0;
        n = n * 10 + (c - '0');
    }
    return n;
}

// The notice a Source kind or a note's origin calls for: content copied from correspondence or conversations.
std::string copied_notice(const std::string& source) {
    if (source == "mail") return "mail";
    if (source == "threads") return "thread";
    if (source == "meetings") return "transcript";
    return {};
}

}  // namespace

// Why a fact can or cannot be published, and with which support.
struct KnowledgeService::Eligibility {
    bool ok = false;
    std::string reason;                // disputed | pending | unsupported | withheld | source_not_permitted
    std::set<std::string> kinds;       // the Source kinds the chosen support needs, required facts included
    std::set<std::string> forbidden;   // source_not_permitted: the kinds policy does not permit
    std::set<std::string> notices;     // mail | chat | thread | transcript
    std::vector<SupportEvidence> excerpts;  // the chosen support's exact excerpts (published as evidence)
    std::vector<long long> evidence_ids;    // every excerpt of the chosen support, recorded locally
};

std::vector<std::string> KnowledgeService::derived_sources_policy() {
    if (env_.policy)
        if (const auto v = env_.policy("knowledge.sharing.derived_sources"); v && v->is_array()) return strings_of(*v);
    return {"files", "notes"};
}

const KnowledgeService::Eligibility& KnowledgeService::publication_eligibility(long long fact_id, const std::set<std::string>& permitted,
                                                                               std::map<long long, Eligibility>& memo, int depth) {
    if (const auto it = memo.find(fact_id); it != memo.end()) return it->second;
    Eligibility& out = memo[fact_id];  // a cycle (refused by the schema) reads as not yet eligible
    out.reason = "unsupported";
    if (depth > kMaxDerivationDepth) return out;
    const auto state = store_->fact_state(fact_id, false);
    if (!state || state->state == "rejected" || state->state == "superseded" || state->state == "unsupported") return out;
    if (state->state == "disputed") {
        out.reason = "disputed";
        return out;
    }
    // Not yet compared with related knowledge: it may still turn out disputed.
    const auto cfg = config();
    if (cfg.extraction && cfg.consistency && !store_->consistency_checked(fact_id)) {
        out.reason = "pending";
        return out;
    }
    struct Candidate {
        Eligibility e;
        size_t exact = 0;
    };
    std::optional<Candidate> best;
    std::optional<std::set<std::string>> fewest_forbidden;
    bool hidden = false;
    for (const auto& set : store_->support_sets(fact_id)) {
        Eligibility e;
        bool usable = true;
        for (const auto& ev : set.evidence) {
            const auto item = store_->item(ev.item_id);
            if (!item || !item_visible(*item)) {
                usable = false;
                hidden = true;
                break;
            }
            e.kinds.insert(ev.source);
            e.evidence_ids.push_back(ev.evidence_id);
            if (ev.precision == "exact") e.excerpts.push_back(ev);
            if (const auto n = copied_notice(ev.source); !n.empty()) e.notices.insert(n);
            if (ev.source == "notes")
                if (const auto note = store_->note(note_id(ev.ext_id)); note && !note->origin_kind.empty())
                    e.notices.insert(note->origin_kind == "chat" ? "chat" : note->origin_kind == "mail" ? "mail" : note->origin_kind);
        }
        if (!usable) continue;
        // Every fact the support requires must be publishable too, by its own permitted support.
        for (long long required : set.required) {
            const Eligibility& r = publication_eligibility(required, permitted, memo, depth + 1);
            if (r.reason == "source_not_permitted") e.forbidden.insert(r.forbidden.begin(), r.forbidden.end());
            else if (!r.ok) usable = false;
            e.kinds.insert(r.kinds.begin(), r.kinds.end());
            e.notices.insert(r.notices.begin(), r.notices.end());
        }
        if (!usable) continue;
        for (const auto& k : e.kinds)
            if (!permitted.count(k)) e.forbidden.insert(k);
        if (!e.forbidden.empty()) {
            if (!fewest_forbidden || e.forbidden.size() < fewest_forbidden->size()) fewest_forbidden = e.forbidden;
            continue;
        }
        if (e.kinds.empty()) continue;  // nothing to name as its Source
        // Prefer the support with the most exact excerpts, then the one needing fewest Source kinds.
        const size_t exact = e.excerpts.size();
        if (!best || exact > best->exact || (exact == best->exact && e.kinds.size() < best->e.kinds.size())) best = Candidate{std::move(e), exact};
    }
    if (best) {
        out = std::move(best->e);
        out.ok = true;
        out.reason.clear();
    } else if (fewest_forbidden) {
        out.reason = "source_not_permitted";
        out.forbidden = *fewest_forbidden;
    } else if (hidden) {
        out.reason = "withheld";
    }
    return out;
}

Result<json> KnowledgeService::publication_options() {
    json out = {{"available", false}};
    if (sharing_policy().empty()) {
        out["reason"] = "policy";
        return out;
    }
    if (!env_.publication) {
        out["reason"] = "unavailable";
        return out;
    }
    const auto unreachable = [&](const Error& e) {
        out["reason"] = e.code == "offline" ? "offline" : "unavailable";
        out["message"] = e.message;
        return out;
    };
    const auto caps = env_.publication("GET", "api/v1/knowledge/capabilities", {});
    if (!caps) return unreachable(caps.error());
    bool supported = false;
    for (const auto& v : caps->value("publication_mutation_versions", json::array()))
        supported = supported || (v.is_number_integer() && v.get<int>() == kPublicationVersion);
    if (!supported) {
        out["reason"] = "unsupported";
        return out;
    }
    const auto mine = env_.publication("GET", "api/v1/knowledge/publications", {});
    if (!mine) return unreachable(mine.error());
    // The server's Groups for this owner, narrowed by the local policy (a wildcard leaves the server's answer).
    const auto policy = sharing_policy();
    const bool any = std::find(policy.begin(), policy.end(), "*") != policy.end();
    json groups = json::array();
    for (const auto& g : strings_of(mine->value("permitted_groups", json::array())))
        if (any || std::find(policy.begin(), policy.end(), g) != policy.end()) groups.push_back(g);
    const auto local = derived_sources_policy();
    json derived = json::array();
    for (const auto& k : strings_of(mine->value("derived_sources", json::array())))
        if (std::find(local.begin(), local.end(), k) != local.end()) derived.push_back(k);
    json categories = json::array();
    for (const auto& c : store_->categories()) categories.push_back({{"id", c.id}, {"name", c.name}});
    out = {{"available", true}, {"groups", groups}, {"derived_sources", derived}, {"categories", categories}};
    const std::string company = jstr(*mine, "company_group");
    if (!company.empty() && std::find(groups.begin(), groups.end(), company) != groups.end()) out["company"] = company;
    return out;
}

Result<json> KnowledgeService::preview_publication(const json& request) {
    auto options = publication_options();
    if (!options) return options.error();
    if (!jbool(*options, "available"))
        return make_error(jstr(*options, "reason"), jstr(*options, "message", "Publishing knowledge is not available now."));
    // The audience: Groups the server and policy permit now.
    const auto offered = strings_of((*options)["groups"]);
    std::vector<std::string> groups;
    for (const auto& g : strings_of(request.value("groups", json::array()))) {
        if (std::find(offered.begin(), offered.end(), g) == offered.end())
            return make_error("GROUP_NOT_PERMITTED", "You cannot publish to " + g + ".");
        if (std::find(groups.begin(), groups.end(), g) == groups.end()) groups.push_back(g);
    }
    if (groups.empty() || groups.size() > kMaxPublicationGroups) return make_error("invalid", "Choose at least one Group.");
    // The categories, in the order chosen.
    std::map<long long, std::string> names;
    for (const auto& c : store_->categories()) names[c.id] = c.name;
    std::vector<long long> categories;
    for (const auto& c : request.value("categories", json::array())) {
        const long long id = c.is_number_integer() ? c.get<long long>() : 0;
        if (!names.count(id)) return make_error("not_found", "A chosen category no longer exists.");
        if (std::find(categories.begin(), categories.end(), id) == categories.end()) categories.push_back(id);
    }
    if (categories.empty()) return make_error("invalid", "Choose at least one category.");
    std::string title = public_label(jstr(request, "title"));
    if (title.empty()) {
        for (long long id : categories) title += (title.empty() ? "" : ", ") + names[id];
        title = public_label(title);
    }
    std::set<long long> deselected;
    for (const auto& f : request.value("exclude", json::array()))
        if (f.is_number_integer()) deselected.insert(f.get<long long>());
    const auto derived = strings_of((*options)["derived_sources"]);
    const std::set<std::string> permitted(derived.begin(), derived.end());
    const auto generation = store_->knowledge_generation();
    if (!generation) return make_error("schema", "Knowledge is not ready for publishing yet.");

    // Each fact once, in the order of the categories chosen; a fact in several of them lists them all.
    std::vector<long long> order;
    std::map<long long, std::vector<std::string>> in_categories;
    for (long long category : categories) {
        long long cursor = 0;
        for (int read = 0; read < kMaxCategoryFacts;) {
            const auto page = store_->fact_page(category, cursor, 200);
            for (long long id : page) {
                if (!in_categories.count(id)) order.push_back(id);
                in_categories[id].push_back(names[category]);
                cursor = id;
            }
            read += static_cast<int>(page.size());
            if (page.size() < 200) break;
        }
    }

    std::map<long long, Eligibility> memo;
    json entries = json::array(), excluded = json::array(), payload_entries = json::array();
    std::map<std::string, long long> notices;
    std::set<std::string> ids;  // entry and evidence ids are unique within a publication
    const auto fresh_id = [&](const char* prefix) {
        std::string id;
        do id = prefix + random_urlsafe(9);
        while (!ids.insert(id).second);
        return id;
    };
    for (long long fact_id : order) {
        const auto state = store_->fact_state(fact_id, false);
        const std::string text = state ? statement(state->fact) : std::string();
        const Eligibility& e = publication_eligibility(fact_id, permitted, memo, 0);
        json shown_categories = in_categories[fact_id];
        if (!e.ok) {
            json x = {{"fact_id", fact_id}, {"reason", e.reason}, {"categories", shown_categories}};
            // The statement is shown to the owner only while one of its items may be read.
            if (e.reason != "withheld" && !described_fact(fact_id).contains("withheld")) x["text"] = text;
            if (!e.forbidden.empty()) x["kinds"] = std::vector<std::string>(e.forbidden.begin(), e.forbidden.end());
            excluded.push_back(std::move(x));
            continue;
        }
        const bool selected = !deselected.count(fact_id) && payload_entries.size() < kMaxPublicationEntries;
        if (!deselected.count(fact_id) && !selected) {
            excluded.push_back({{"fact_id", fact_id}, {"reason", "limit"}, {"text", text}, {"categories", shown_categories}});
            continue;
        }
        json evidence = json::array(), shown_evidence = json::array(), local = json::array();
        std::set<long long> quoted;
        for (const auto& x : e.excerpts) {
            if (!quoted.insert(x.evidence_id).second || trim(x.excerpt).empty()) continue;
            const std::string id = selected ? fresh_id("x-") : std::string();
            evidence.push_back({{"id", id}, {"title", public_title(x)}, {"text", x.excerpt}});
            shown_evidence.push_back({{"title", public_title(x)}, {"text", x.excerpt}, {"source", x.source}});
            local.push_back({{"id", id}, {"evidence_id", x.evidence_id}});
        }
        const std::vector<std::string> kinds(e.kinds.begin(), e.kinds.end());
        json entry = {{"fact_id", fact_id}, {"text", text}, {"source_kinds", kinds}, {"evidence", shown_evidence}, {"categories", shown_categories},
                      {"selected", selected}};
        if (!e.notices.empty()) entry["notices"] = std::vector<std::string>(e.notices.begin(), e.notices.end());
        if (selected) {
            const std::string entry_id = fresh_id("e-");
            json published = {{"id", entry_id}, {"text", text}, {"source_kinds", kinds}, {"evidence", evidence}};
            entry["entry_id"] = entry_id;
            entry["sha256"] = sha256_hex(published.dump());
            entry["local_evidence"] = local;
            for (const auto& n : e.notices) ++notices[n];
            payload_entries.push_back(std::move(published));
        }
        entries.push_back(std::move(entry));
    }

    // The payload exactly as it will be uploaded: nothing else of the categories, no paths, no local ids.
    const std::string publication_id = "p-" + random_urlsafe(12);
    std::string payload;
    if (!payload_entries.empty())
        payload = json{{"expected_revision", 0},
                       {"operation_id", "o-" + random_urlsafe(12)},
                       {"title", title.empty() ? "Knowledge" : title},
                       {"groups", groups},
                       {"entries", payload_entries}}
                      .dump();
    json notice_list = json::array();
    for (const auto& [kind, n] : notices) notice_list.push_back({{"kind", kind}, {"entries", n}});
    json summary = {{"publication_id", publication_id},
                    {"title", title.empty() ? "Knowledge" : title},
                    {"groups", groups},
                    {"categories", categories},
                    {"entries", entries},
                    {"excluded", excluded},
                    {"notices", notice_list},
                    {"selected", payload_entries.size()},
                    {"update", "review"}};
    if (options->contains("company") && std::find(groups.begin(), groups.end(), jstr(*options, "company")) != groups.end())
        summary["company"] = (*options)["company"];
    if (payload.empty()) {
        summary["payload"] = nullptr;  // nothing to publish: no preview to confirm
        return summary;
    }
    OperationPreview p;
    p.operation = "publish";
    p.target = json{{"publication_id", publication_id}, {"categories", categories}, {"groups", groups}}.dump();
    p.content = payload;
    p.content_hash = sha256_hex(payload);
    p.policy = json{{"derived", derived}, {"groups", sharing_policy()}}.dump();
    p.summary = summary.dump();
    p.generation = *generation;
    p.created_at = now_ms();
    auto token = store_->save_publication_preview(p);
    if (!token) return token.error();
    summary["token"] = *token;
    summary["payload"] = payload;
    return summary;
}

Result<json> KnowledgeService::confirm_publication(const std::string& token) {
    const auto p = store_->publication_preview(token);
    if (!p) return make_error("not_found", "The preview is no longer available; preview the publication again.");
    const auto stale = [&](const std::string& why) {
        (void)store_->delete_publication_preview(token);
        return make_error("stale", why);
    };
    if (p->expires_at <= now_ms()) return stale("The preview expired; preview the publication again.");
    // What was reviewed must still be what is uploaded: no knowledge or sharing policy changed since.
    if (store_->knowledge_generation() != p->generation) return stale("Knowledge changed since the preview; review the new preview.");
    const json summary = parse_json(p->summary).value_or(json::object());
    const json target = parse_json(p->target).value_or(json::object());
    const auto local = derived_sources_policy();
    std::vector<std::string> derived;
    for (const auto& k : strings_of(json::parse(p->policy, nullptr, false).value("derived", json::array())))
        if (std::find(local.begin(), local.end(), k) != local.end()) derived.push_back(k);
    if (json{{"derived", derived}, {"groups", sharing_policy()}}.dump() != p->policy)
        return stale("Sharing policy changed since the preview; preview the publication again.");
    if (!env_.publication) return make_error("unavailable", "Publishing knowledge is not available now.");
    const std::string id = jstr(target, "publication_id");
    // The previewed bytes, unchanged; a retry after a lost answer repeats the same operation and is not applied twice.
    auto r = env_.publication("PUT", "api/v1/knowledge/publications/" + id, p->content);
    if (!r) {
        if (r.error().code == "CONFLICT" || r.error().code == "http_409") return stale(r.error().message);
        return r.error();
    }
    PublicationRow row;
    row.id = id;
    row.title = jstr(summary, "title");
    row.groups = strings_of(summary.value("groups", json::array()));
    for (const auto& c : summary.value("categories", json::array()))
        if (c.is_number_integer()) row.categories.push_back(c.get<long long>());
    row.revision = jint(*r, "revision");
    row.payload_sha256 = p->content_hash;
    row.published_at = now_ms();
    for (const auto& e : summary.value("entries", json::array()))
        if (jbool(e, "selected")) row.entries.push_back({jstr(e, "entry_id"), jint(e, "fact_id"), jstr(e, "sha256"), e.value("local_evidence", json::array())});
    (void)store_->delete_publication_preview(token);
    if (Status s = store_->record_publication(row); !s) return s.error();  // published; the local record failed
    notify();
    return json{{"publication_id", id}, {"revision", row.revision}, {"entries", row.entries.size()}, {"groups", row.groups}};
}

Status KnowledgeService::cancel_publication(const std::string& token) { return store_->delete_publication_preview(token); }

json KnowledgeService::publications() {
    json out = json::array();
    for (const auto& p : store_->publications())
        out.push_back({{"publication_id", p.id}, {"title", p.title}, {"groups", p.groups}, {"categories", p.categories},
                       {"revision", p.revision}, {"published_at", p.published_at}, {"entries", p.entries.size()}});
    return {{"publications", out}};
}

}  // namespace wb::knowledge
