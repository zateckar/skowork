// Category publications of client/knowledge-graph (Category Publication Eligibility, Category Publication Preview,
// Publication Change Control, Honest Sharing Synchronization; improve-skowork-and-knowledge-control design D8, tasks
// 10.2 and 10.3): which facts of chosen categories may be published, the exact payload control-plane/team-knowledge
// receives and its upload after confirmation, the next reviewed version, and the withdrawal of published entries that
// no longer hold, queued before anything is added and retried until the server acknowledges it.
#include "knowledge/knowledge_service.h"

#include "base/encoding.h"
#include "platform/log.h"
#include "base/strings.h"
#include "platform/crypto.h"

#include <algorithm>
#include <map>
#include <set>

namespace wb::knowledge {

namespace {

constexpr char kLog[] = "knowledge";
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

// What recipients see of an entry, without its ids: unchanged exactly when this digest is.
std::string entry_digest(const std::string& text, const std::vector<std::string>& kinds, const json& excerpts) {
    return sha256_hex(json{{"text", text}, {"source_kinds", kinds}, {"evidence", excerpts}}.dump());
}

bool is_code(const Error& e, const char* code, const char* http) { return e.code == code || e.code == http; }

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
                                                                               std::map<long long, Eligibility>& memo, int depth, bool checked) {
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
    if (checked && cfg.extraction && cfg.consistency && !store_->consistency_checked(fact_id)) {
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
            const Eligibility& r = publication_eligibility(required, permitted, memo, depth + 1, checked);
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
    // The next version of an existing publication: withdrawals from it are acknowledged first (design D8).
    std::optional<PublicationRow> current;
    if (const std::string id = jstr(request, "publication_id"); !id.empty()) {
        if (!store_->publication(id)) return make_error("not_found", "This publication no longer exists.");
        review_publications();
        const bool clear = sync_publications(true, id);
        current = store_->publication(id);
        if (!current) return make_error("not_found", "This publication no longer exists.");
        if (current->state == "removed")
            return make_error("removed", "An administrator removed this publication. It stays removed until an administrator resolves it.");
        if (current->state == "withdrawing") return make_error("withdrawal_pending", "This publication is being withdrawn.");
        if (!clear)
            return make_error("withdrawal_pending",
                              "Earlier removals from this publication are not confirmed by the server yet. Review it again once they are.");
    }
    // The audience: Groups the server and policy permit now.
    const auto offered = strings_of((*options)["groups"]);
    std::vector<std::string> groups;
    const json chosen_groups = request.contains("groups") || !current ? request.value("groups", json::array()) : json(current->groups);
    for (const auto& g : strings_of(chosen_groups)) {
        if (std::find(offered.begin(), offered.end(), g) == offered.end())
            return make_error("GROUP_NOT_PERMITTED", "You cannot publish to " + g + ".");
        if (std::find(groups.begin(), groups.end(), g) == groups.end()) groups.push_back(g);
    }
    if (groups.empty() || groups.size() > kMaxPublicationGroups) return make_error("invalid", "Choose at least one Group.");
    // The categories, in the order chosen.
    std::map<long long, std::string> names;
    for (const auto& c : store_->categories()) names[c.id] = c.name;
    std::vector<long long> categories;
    const json chosen_categories = request.contains("categories") || !current ? request.value("categories", json::array()) : json(current->categories);
    for (const auto& c : chosen_categories) {
        const long long id = c.is_number_integer() ? c.get<long long>() : 0;
        if (!names.count(id)) return make_error("not_found", "A chosen category no longer exists.");
        if (std::find(categories.begin(), categories.end(), id) == categories.end()) categories.push_back(id);
    }
    if (categories.empty()) return make_error("invalid", "Choose at least one category.");
    std::string title = public_label(jstr(request, "title"));
    if (title.empty() && current) title = public_label(current->title);
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
    // Entries still published keep their ids, and so do their excerpts.
    std::map<long long, const PublicationEntryRow*> published;
    if (current)
        for (const auto& e : current->entries)
            if (e.state == "published" && e.fact_id) {
                published[e.fact_id] = &e;
                ids.insert(e.entry_id);
                for (const auto& x : e.evidence) ids.insert(jstr(x, "id"));
            }
    const auto fresh_id = [&](const char* prefix) {
        std::string id;
        do id = prefix + random_urlsafe(9);
        while (!ids.insert(id).second);
        return id;
    };
    long long kept = 0, added = 0;
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
        const std::vector<std::string> kinds(e.kinds.begin(), e.kinds.end());
        json excerpts = json::array(), shown_evidence = json::array();
        std::vector<const SupportEvidence*> quoted_excerpts;
        std::set<long long> quoted;
        for (const auto& x : e.excerpts) {
            if (!quoted.insert(x.evidence_id).second || trim(x.excerpt).empty()) continue;
            excerpts.push_back({{"title", public_title(x)}, {"text", x.excerpt}});
            shown_evidence.push_back({{"title", public_title(x)}, {"text", x.excerpt}, {"source", x.source}});
            quoted_excerpts.push_back(&x);
        }
        const std::string digest = entry_digest(text, kinds, excerpts);
        // An entry published as it is now keeps its ids; anything else is new and waits for this review.
        const auto was = published.find(fact_id);
        const PublicationEntryRow* same = was != published.end() && was->second->text_sha256 == digest ? was->second : nullptr;
        json entry = {{"fact_id", fact_id}, {"text", text}, {"source_kinds", kinds}, {"evidence", shown_evidence}, {"categories", shown_categories},
                      {"selected", selected}};
        if (current) entry["change"] = same ? "kept" : "new";
        if (!e.notices.empty()) entry["notices"] = std::vector<std::string>(e.notices.begin(), e.notices.end());
        if (selected) {
            std::map<long long, std::string> old_ids;
            if (same)
                for (const auto& x : same->evidence) old_ids[jint(x, "evidence_id")] = jstr(x, "id");
            json evidence = json::array(), local = json::array();
            for (size_t i = 0; i < quoted_excerpts.size(); ++i) {
                const auto old = old_ids.find(quoted_excerpts[i]->evidence_id);
                const std::string id = old != old_ids.end() && !old->second.empty() ? old->second : fresh_id("x-");
                evidence.push_back({{"id", id}, {"title", excerpts[i]["title"]}, {"text", excerpts[i]["text"]}});
                local.push_back({{"id", id}, {"evidence_id", quoted_excerpts[i]->evidence_id}});
            }
            const std::string entry_id = same ? same->entry_id : fresh_id("e-");
            json uploaded = {{"id", entry_id}, {"text", text}, {"source_kinds", kinds}, {"evidence", evidence}};
            entry["entry_id"] = entry_id;
            entry["sha256"] = sha256_hex(uploaded.dump());
            entry["digest"] = digest;
            entry["local_evidence"] = local;
            for (const auto& n : e.notices) ++notices[n];
            ++(same ? kept : added);
            payload_entries.push_back(std::move(uploaded));
        }
        entries.push_back(std::move(entry));
    }

    // The payload exactly as it will be uploaded: nothing else of the categories, no paths, no local ids.
    const std::string publication_id = current ? current->id : "p-" + random_urlsafe(12);
    const long long revision = current ? current->revision : 0;
    std::string payload;
    if (!payload_entries.empty())
        payload = json{{"expected_revision", revision},
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
    if (current) {
        // What changes for recipients: entries added, entries no longer published, Groups added.
        json added_groups = json::array();
        for (const auto& g : groups)
            if (std::find(current->groups.begin(), current->groups.end(), g) == current->groups.end()) added_groups.push_back(g);
        long long published_now = 0;
        for (const auto& e : current->entries) published_now += e.state == "published";
        summary["revision"] = revision;
        summary["kept"] = kept;
        summary["new"] = added;
        summary["removed"] = published_now - kept;
        summary["added_groups"] = added_groups;
    }
    if (options->contains("company") && std::find(groups.begin(), groups.end(), jstr(*options, "company")) != groups.end())
        summary["company"] = (*options)["company"];
    if (payload.empty()) {
        summary["payload"] = nullptr;  // nothing to publish: no preview to confirm
        return summary;
    }
    OperationPreview p;
    p.operation = "publish";
    p.target = json{{"publication_id", publication_id}, {"categories", categories}, {"groups", groups}, {"revision", revision},
                    {"update", current.has_value()}}
                   .dump();
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
    const std::lock_guard lock(publication_mx_);
    // A next version replaces the version previewed against, with no withdrawal from it still queued.
    if (jbool(target, "update")) {
        const auto now = store_->publication(id);
        const bool queued = now && std::any_of(now->entries.begin(), now->entries.end(), [](const auto& e) { return e.state == "withdrawing"; });
        if (!now || now->revision != jint(target, "revision") || now->state == "removed" || now->state == "withdrawing" || queued)
            return stale("The publication changed since the preview; review it again.");
    }
    // The previewed bytes, unchanged; a retry after a lost answer repeats the same operation and is not applied twice.
    auto r = env_.publication("PUT", "api/v1/knowledge/publications/" + id, p->content);
    if (!r) {
        if (is_code(r.error(), "CONFLICT", "http_409")) return stale(r.error().message);
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
        if (jbool(e, "selected")) {
            PublicationEntryRow entry;
            entry.entry_id = jstr(e, "entry_id");
            entry.fact_id = jint(e, "fact_id");
            entry.text_sha256 = jstr(e, "digest");
            entry.evidence = e.value("local_evidence", json::array());
            row.entries.push_back(std::move(entry));
        }
    (void)store_->delete_publication_preview(token);
    if (Status s = store_->record_publication(row); !s) return s.error();  // published; the local record failed
    reviewed_generation_ = -1;  // review the new version at the next step
    notify();
    return json{{"publication_id", id}, {"revision", row.revision}, {"entries", row.entries.size()}, {"groups", row.groups}};
}

Status KnowledgeService::cancel_publication(const std::string& token) { return store_->delete_publication_preview(token); }

bool KnowledgeService::review_publications(bool always) {
    const std::lock_guard lock(publication_mx_);
    const auto generation = store_->knowledge_generation();
    if (!generation || (!always && *generation == reviewed_generation_)) return false;
    const auto pubs = store_->publications();
    reviewed_generation_ = *generation;
    if (pubs.empty()) return false;
    const auto derived = derived_sources_policy();
    const std::set<std::string> permitted(derived.begin(), derived.end());
    std::map<long long, Eligibility> memo;
    std::map<long long, std::set<long long>> members;  // category -> its facts
    const auto member = [&](long long category, long long fact) {
        if (!members.count(category)) {
            auto& set = members[category];
            long long cursor = 0;
            for (int read = 0; read < kMaxCategoryFacts;) {
                const auto page = store_->fact_page(category, cursor, 200);
                for (long long id : page) set.insert(cursor = id);
                read += static_cast<int>(page.size());
                if (page.size() < 200) break;
            }
        }
        return members[category].count(fact) > 0;
    };
    std::vector<PublicationWithdrawal> withdraw;
    for (const auto& p : pubs) {
        if (p.state != "published" && p.state != "removed") continue;
        for (const auto& e : p.entries) {
            if (e.state != "published") continue;
            std::string reason;
            if (!e.fact_id) reason = "forgotten";
            else if (std::none_of(p.categories.begin(), p.categories.end(), [&](long long c) { return member(c, e.fact_id); })) reason = "category";
            else {
                // Not waiting for a comparison with related knowledge: a published fact is withdrawn when it no longer
                // holds, not while it is being checked.
                const Eligibility& el = publication_eligibility(e.fact_id, permitted, memo, 0, false);
                if (!el.ok) reason = el.reason;
                else {
                    const auto state = store_->fact_state(e.fact_id, false);
                    json excerpts = json::array();
                    std::set<long long> quoted;
                    for (const auto& x : el.excerpts)
                        if (quoted.insert(x.evidence_id).second && !trim(x.excerpt).empty())
                            excerpts.push_back({{"title", public_title(x)}, {"text", x.excerpt}});
                    const std::vector<std::string> kinds(el.kinds.begin(), el.kinds.end());
                    if (!state || entry_digest(statement(state->fact), kinds, excerpts) != e.text_sha256) reason = "changed";
                }
            }
            if (!reason.empty()) withdraw.push_back({p.id, e.entry_id, reason});
        }
    }
    if (withdraw.empty()) return false;
    if (Status s = store_->queue_entry_withdrawals(withdraw); !s) {
        log::warn(kLog, "Could not queue publication withdrawals: " + s.error().message);
        reviewed_generation_ = -1;
        return false;
    }
    return true;
}

void KnowledgeService::refresh_publications(const json& mine) {
    std::set<std::string> local;
    for (const auto& p : store_->publications()) local.insert(p.id);
    for (const auto& p : mine.value("publications", json::array())) {
        const std::string id = jstr(p, "publication_id");
        if (!local.count(id)) continue;
        const json removal = p.value("removal", json());
        (void)store_->set_publication_server_state(id, jint(p, "revision"), jstr(p, "state"),
                                                   removal.is_object() ? jstr(removal, "reason") : std::string());
    }
}

bool KnowledgeService::sync_publications(bool list, const std::string& only) {
    const std::lock_guard lock(publication_mx_);
    const auto pending = [&] {
        bool any = false;
        for (const auto& p : store_->publications())
            if (only.empty() || p.id == only)
                any = any || p.state == "withdrawing" ||
                      std::any_of(p.entries.begin(), p.entries.end(), [](const auto& e) { return e.state == "withdrawing"; });
        return any;
    };
    const bool queued = pending();
    if (!env_.publication || (!queued && !list) || store_->publications().empty()) return !queued;
    const auto fail = [&](const std::string& why) {
        for (const auto& p : store_->publications())
            if (only.empty() || p.id == only) (void)store_->set_publication_error(p.id, why);
        notify();
        return !pending();
    };
    // A removal answered "not found" counts as done only from a server that has publications on.
    const auto caps = env_.publication("GET", "api/v1/knowledge/capabilities", {});
    if (!caps) return fail(caps.error().message);
    bool supported = false;
    for (const auto& v : caps->value("publication_mutation_versions", json::array()))
        supported = supported || (v.is_number_integer() && v.get<int>() == kPublicationVersion);
    if (!supported) return fail("The server does not accept publication changes now.");
    const auto list_mine = [&] {
        if (auto mine = env_.publication("GET", "api/v1/knowledge/publications", {}); mine) refresh_publications(*mine);
    };
    if (list) list_mine();
    const auto send = [&](const std::string& id, const std::string& entry) {
        for (int attempt = 0; attempt < 2; ++attempt) {
            const auto revision = store_->withdrawal_revision(id, entry);
            if (!revision) return;  // no longer queued
            const auto p = store_->publication(id);
            if (!p) return;
            std::string operation = p->operation_id;
            if (!entry.empty())
                for (const auto& e : p->entries)
                    if (e.entry_id == entry) operation = e.operation_id;
            const std::string body = json{{"expected_revision", *revision}, {"operation_id", operation}}.dump();
            auto r = env_.publication("DELETE", "api/v1/knowledge/publications/" + id + (entry.empty() ? "" : "/entries/" + entry), body);
            if (r) {
                (void)store_->acknowledge_withdrawal(id, entry, jint(*r, "current_revision", jint(*r, "revision")));
                return;
            }
            if (is_code(r.error(), "NOT_FOUND", "http_404")) {  // nothing of it is on the server
                (void)store_->acknowledge_withdrawal(id, entry, p->revision);
                return;
            }
            if (attempt == 0 && is_code(r.error(), "CONFLICT", "http_409")) {
                // The revision moved (an administrator, another device): send it again against the current one.
                list_mine();
                (void)store_->restart_withdrawal(id, entry);
                continue;
            }
            (void)store_->set_publication_error(id, r.error().message);
            return;
        }
    };
    for (const auto& p : store_->publications()) {
        if (!only.empty() && p.id != only) continue;
        if (p.state == "withdrawing") {
            send(p.id, {});
            continue;
        }
        for (const auto& e : p.entries)
            if (e.state == "withdrawing") send(p.id, e.entry_id);
    }
    notify();
    return !pending();
}

Result<json> KnowledgeService::withdraw_publication(const std::string& id) {
    const auto p = store_->publication(id);
    if (!p) return make_error("not_found", "This publication no longer exists.");
    if (p->state == "published")
        if (Status s = store_->queue_publication_withdrawal(id); !s) return s.error();
    const bool done = sync_publications(false, id);
    const auto now = store_->publication(id);
    return json{{"state", now ? now->state : "withdrawn"}, {"pending", !done}};
}

Status KnowledgeService::withdraw_publications_for_delete_all() {
    for (const auto& p : store_->publications())
        if (p.state == "published")
            if (Status s = store_->queue_publication_withdrawal(p.id); !s) return s;
    if (sync_publications(false)) return {};
    return make_error("remote_pending", "Published knowledge could not be withdrawn. Reconnect and try Delete all again; your local data has not been deleted.");
}

json KnowledgeService::publications() {
    const auto derived = derived_sources_policy();
    const std::set<std::string> permitted(derived.begin(), derived.end());
    std::map<long long, Eligibility> memo;
    json out = json::array();
    for (const auto& p : store_->publications()) {
        json withdrawing = json::array();
        std::set<long long> published;
        long long entries = 0;
        for (const auto& e : p.entries) {
            if (e.state == "published") {
                ++entries;
                published.insert(e.fact_id);
                continue;
            }
            json w = {{"entry_id", e.entry_id}, {"reason", e.reason}};
            // The statement while the fact exists and one of its items may be read.
            if (e.fact_id)
                if (const auto f = store_->fact_state(e.fact_id, false); f && !described_fact(e.fact_id).contains("withheld"))
                    w["text"] = statement(f->fact);
            withdrawing.push_back(std::move(w));
        }
        // Facts of its categories that could be published and are not: they wait for review.
        long long fresh = 0;
        if (p.state == "published") {
            std::set<long long> seen;
            for (long long category : p.categories) {
                long long cursor = 0;
                for (int read = 0; read < kMaxCategoryFacts;) {
                    const auto page = store_->fact_page(category, cursor, 200);
                    for (long long id : page) {
                        cursor = id;
                        if (seen.insert(id).second && !published.count(id) && publication_eligibility(id, permitted, memo, 0).ok) ++fresh;
                    }
                    read += static_cast<int>(page.size());
                    if (page.size() < 200) break;
                }
            }
        }
        json row = {{"publication_id", p.id}, {"title", p.title}, {"groups", p.groups}, {"categories", p.categories},
                    {"revision", p.revision}, {"published_at", p.published_at}, {"state", p.state}, {"entries", entries},
                    {"withdrawing", withdrawing}, {"new", fresh}};
        if (!p.removal.empty()) row["removal"] = p.removal;
        if (!p.error.empty() && (p.state == "withdrawing" || !withdrawing.empty())) row["error"] = p.error;
        out.push_back(std::move(row));
    }
    return {{"publications", out}};
}

}  // namespace wb::knowledge