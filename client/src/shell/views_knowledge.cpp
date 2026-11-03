// Knowledge sources (client/knowledge-graph Knowledge Status And Control): the Settings section with each Source's state,
// counts, last update, pending work and errors, and pause, re-index and forget; the indexed folders; policy's exclusions,
// which cannot be removed, and the user's own; the budget and backlog; and forgetting an item, and forgetting, merging and
// splitting entities. The host does the work (knowledge.status, .source.*, .folder.*, .exclusion.*, .items, .item.forget,
// .entities, .entity, .entity.forget, .entity.merge, .entity.split).
#include "platform/win.h"
#include "shell/shell.h"
#include "ui/text16.h"
#include "ui/textfield.h"

namespace wb::shell {

using ui::Node;
using ui::Paint;
using ui::TextStyle;

namespace {

std::u16string u16(const std::string& s) { return ui::to_utf16(s); }
std::u16string num(long long n) { return u16(std::to_string(n)); }

// "2026-10-26T15:04:05.123Z" as "2026-10-26 15:04 UTC".
std::u16string when(const std::string& iso) {
    if (iso.size() < 16) return u16(iso);
    return u16(iso.substr(0, 10) + " " + iso.substr(11, 5) + " UTC");
}

// A text field whose text survives page rebuilds in `keep`.
ui::TextField* kept_field(Shell& shell, Node& parent, const std::string& id, const std::u16string& label, std::u16string& keep) {
    auto* field = parent.emplace<ui::TextField>(shell.ui(), id, label);
    field->set_text(keep);
    ui::TextField* fp = field;
    field->on_change = [&keep, fp] { keep = fp->text(); };
    return field;
}

bool blank(const std::u16string& s) { return s.find_first_not_of(u" \t") == std::u16string::npos; }

Node* row_box(Node& parent, const std::string& id = {}) {
    auto* r = id.empty() ? parent.emplace<Node>() : parent.emplace<Node>(id);
    r->axis = ui::Axis::row;
    r->gap = 8;
    return r;
}

}  // namespace

void PagePane::load_knowledge() {
    std::weak_ptr<int> alive = alive_;
    shell_.call("knowledge.status", json::object(), [this, alive](Result<json> res) {
        if (!alive.lock()) return;
        if (res) {
            knowledge_ = *res;
            errors_.erase("knowledge");
        } else {
            knowledge_ = nullptr;
            errors_["knowledge"] = u16(res.error().message);
        }
        rebuild();
    });
}

void PagePane::knowledge_changed() {
    if (shell_.route().name == RouteName::knowledge && knowledge_view_) knowledge_view_->changed();
    if (shell_.route().name != RouteName::settings) return;
    load_knowledge();
    if (knowledge_entity_.is_object()) open_entity(jint(knowledge_entity_, "id"));
}

void PagePane::exclude_knowledge_files(std::vector<std::string> paths) {
    if (paths.empty()) return;
    const std::string path = paths.front();
    paths.erase(paths.begin());
    act("knowledge", "knowledge.exclusion.add", {{"kind", "item"}, {"value", "files:" + path}}, [this, rest = std::move(paths)](const json&) {
        if (rest.empty()) load_knowledge();
        else exclude_knowledge_files(rest);
    });
}

void PagePane::find_knowledge(bool entities) {
    std::weak_ptr<int> alive = alive_;
    const std::string query = ui::to_utf8(entities ? knowledge_entity_query_ : knowledge_item_query_);
    shell_.call(entities ? "knowledge.entities" : "knowledge.items", {{"query", query}, {"limit", 20}},
                [this, alive, entities](Result<json> res) {
                    if (!alive.lock()) return;
                    json& out = entities ? knowledge_entities_ : knowledge_items_;
                    if (res) {
                        out = (*res)[entities ? "entities" : "items"];
                        errors_.erase("knowledge");
                    } else {
                        errors_["knowledge"] = u16(res.error().message);
                    }
                    rebuild();
                });
}

void PagePane::open_entity(long long id) {
    std::weak_ptr<int> alive = alive_;
    shell_.call("knowledge.entity", {{"id", id}}, [this, alive](Result<json> res) {
        if (!alive.lock()) return;
        knowledge_entity_ = res ? *res : json(nullptr);
        rebuild();
    });
}

// Forgetting cannot be undone: ask first.
void PagePane::ask_forget(const std::string& id, const std::u16string& what, const std::string& method, const json& params,
                          std::function<void()> done) {
    auto d = std::make_unique<ui::Dialog>(shell_.ui(), "knowledge-forget-dialog", shell_.t("settings.knowledge.forget_title"));
    d->body().emplace<ui::Label>(shell_.ui(), shell_.t("settings.knowledge.forget_body", {{"name", what}}));
    ui::Dialog* dp = d.get();
    d->add_button("knowledge-forget-cancel", shell_.t("settings.cancel"), ui::Button::Kind::standard, [dp] { dp->close(); });
    d->add_button("knowledge-forget-confirm", shell_.t("settings.knowledge.forget"), ui::Button::Kind::primary,
                  [this, dp, id, method, params, done = std::move(done)] {
                      dp->close();
                      act(id, method, params, [this, done](const json&) {
                          shell_.announce(shell_.t("settings.knowledge.forgotten"));
                          if (done) done();
                          load_knowledge();
                      });
                  });
    shell_.window().modal().show(std::move(d));
    shell_.window().render();
}

void PagePane::build_knowledge(Node& c) {
    text(c, shell_.t("settings.knowledge.body"), Paint::text_secondary);
    if (!knowledge_.is_object()) {
        if (!errors_.contains("knowledge")) text(c, shell_.t("common.loading"), Paint::text_secondary);
        error_line(c, "knowledge");
        return;
    }
    if (jbool(knowledge_, "deferred"))
        text(c, shell_.t("settings.knowledge.deferred"), Paint::text_secondary, "settings-knowledge-deferred");

    // ---- Sources ----
    auto* sources = c.emplace<Node>("settings-knowledge-sources");
    sources->role = ui::Role::list;
    sources->name = shell_.t("settings.knowledge.sources");
    sources->gap = 12;
    if (const json* list = jobj(knowledge_, "sources"); list && list->is_array())
        for (const auto& s : *list) {
            const std::string kind = jstr(s, "kind");
            const std::u16string label = shell_.t("settings.knowledge.source." + kind);
            const bool policy_on = jstr(s, "policy") == "on";
            const bool user_on = jbool(s, "enabled", true);
            const bool paused = jbool(s, "paused");
            std::string state = !policy_on ? "policy_off" : !user_on ? "off" : paused ? "paused" : "on";
            auto* item = sources->emplace<Node>("knowledge-source-" + kind);
            item->role = ui::Role::list_item;
            item->gap = 4;
            item->name = label + u": " + shell_.t("settings.knowledge.states." + state);
            item->emplace<ui::Label>(shell_.ui(), item->name, TextStyle::body_strong, Paint::text, false)->role = ui::Role::none;
            const std::string last = jstr(s, "last_update");
            text(*item,
                 shell_.t("settings.knowledge.counts", {{"items", num(jint(s, "items"))},
                                                        {"pending", num(jint(s, "pending"))},
                                                        {"last", last.empty() ? shell_.t("settings.mail.never") : when(last)}}),
                 Paint::text_secondary, "knowledge-source-counts-" + kind);
            if (const std::string e = jstr(s, "error"); !e.empty())
                text(*item, shell_.t("settings.knowledge.source_error", {{"reason", u16(e)}}), Paint::warning, "knowledge-source-error-" + kind);
            if (const long long n = jint(s, "skipped_count"); n > 0) {
                text(*item, shell_.t("settings.knowledge.skipped", {{"count", num(n)}}), Paint::text_secondary, "knowledge-skipped-" + kind);
                int shown = 0;
                if (const json* sk = jobj(s, "skipped"); sk && sk->is_array())
                    for (const auto& x : *sk) {
                        if (++shown > 10) break;
                        text(*item, u16(jstr(x, "id")) + u" — " + u16(jstr(x, "reason")), Paint::text_secondary);
                    }
            }
            auto* actions = row_box(*item);
            if (!policy_on) {
                text(*actions, shell_.t("settings.knowledge.policy_off"), Paint::text_secondary, "knowledge-source-policy-" + kind);
            } else {
                button(*actions, "knowledge-source-toggle-" + kind, shell_.t(user_on ? "settings.knowledge.turn_off" : "settings.knowledge.turn_on"),
                       [this, kind, user_on] { act("knowledge", "knowledge.source.set", {{"kind", kind}, {"enabled", !user_on}}, [this](const json&) { load_knowledge(); }); })
                    ->enabled = busy_.empty();
                if (user_on)
                    button(*actions, "knowledge-source-pause-" + kind, shell_.t(paused ? "settings.knowledge.resume" : "settings.knowledge.pause"),
                           [this, kind, paused] { act("knowledge", "knowledge.source.pause", {{"kind", kind}, {"paused", !paused}}, [this](const json&) { load_knowledge(); }); })
                        ->enabled = busy_.empty();
                button(*actions, "knowledge-source-reindex-" + kind, shell_.t("settings.knowledge.reindex"), [this, kind] {
                    act("knowledge", "knowledge.source.reindex", {{"kind", kind}}, [this](const json&) {
                        shell_.announce(shell_.t("settings.knowledge.reindex_started"));
                        load_knowledge();
                    });
                })->enabled = busy_.empty();
            }
            auto* forget = button(*actions, "knowledge-source-forget-" + kind, shell_.t("settings.knowledge.forget"),
                                  [this, kind, label] { ask_forget("knowledge", label, "knowledge.source.forget", {{"kind", kind}}); },
                                  ui::Button::Kind::subtle);
            forget->name = shell_.t("settings.knowledge.forget_named", {{"name", label}});
            forget->enabled = busy_.empty() && jint(s, "items") > 0;
        }

    // ---- Folders of the files Source ----
    c.emplace<ui::Label>(shell_.ui(), shell_.t("settings.knowledge.folders"), TextStyle::body_strong)->role = ui::Role::heading;
    auto* folders = c.emplace<Node>("settings-knowledge-folders");
    folders->role = ui::Role::list;
    folders->name = shell_.t("settings.knowledge.folders");
    folders->gap = 4;
    bool any_folder = false;
    if (const json* fl = jobj(knowledge_, "folders"); fl && fl->is_array())
        for (const auto& f : *fl) {
            if (!f.is_string()) continue;
            any_folder = true;
            const std::string path = f.get<std::string>();
            auto* item = row_box(*folders, "knowledge-folder-" + path);
            item->role = ui::Role::list_item;
            item->name = u16(path);
            item->emplace<ui::Label>(shell_.ui(), u16(path), TextStyle::body, Paint::text, false)->role = ui::Role::none;
            auto* rm = button(*item, "knowledge-folder-remove-" + path, shell_.t("settings.mail.remove"), [this, path] {
                act("knowledge", "knowledge.folder.remove", {{"path", path}}, [this](const json&) { load_knowledge(); });
            }, ui::Button::Kind::subtle);
            rm->name = shell_.t("settings.mail.remove_named", {{"name", u16(path)}});
            rm->enabled = busy_.empty();
        }
    if (!any_folder) text(*folders, shell_.t("settings.knowledge.no_folders"), Paint::text_secondary, "settings-knowledge-no-folders");
    kept_field(shell_, c, "settings-knowledge-folder", shell_.t("settings.knowledge.folder_path"), knowledge_folder_);
    button(c, "settings-knowledge-add-folder", shell_.t("settings.knowledge.add_folder"), [this] {
        if (blank(knowledge_folder_)) return;
        act("knowledge", "knowledge.folder.add", {{"path", ui::to_utf8(knowledge_folder_)}}, [this](const json&) {
            knowledge_folder_.clear();
            load_knowledge();
        });
    })->enabled = busy_.empty();

    // ---- Exclusions: policy's first (fixed), then the user's (removable) ----
    c.emplace<ui::Label>(shell_.ui(), shell_.t("settings.mail.exclusions"), TextStyle::body_strong)->role = ui::Role::heading;
    text(c, shell_.t("settings.knowledge.exclusions_body"), Paint::text_secondary);
    const json empty = json::object();
    const json* ex = jobj(knowledge_, "exclusions");
    if (!ex) ex = &empty;
    std::u16string policy_globs;
    if (const json* pol = jobj(*ex, "policy"); pol && pol->is_array())
        for (const auto& g : *pol)
            if (g.is_string()) policy_globs += (policy_globs.empty() ? u"" : u", ") + u16(g.get<std::string>());
    if (!policy_globs.empty()) row(c, "settings-knowledge-policy-exclusions", shell_.t("settings.knowledge.policy_exclusions"), policy_globs);
    auto* user = c.emplace<Node>("settings-knowledge-exclusions");
    user->role = ui::Role::list;
    user->name = shell_.t("settings.mail.your_exclusions");
    user->gap = 4;
    bool any_ex = false;
    if (const json* ul = jobj(*ex, "user"); ul && ul->is_array())
        for (const auto& x : *ul) {
            any_ex = true;
            const std::string kind = jstr(x, "kind"), value = jstr(x, "value");
            const std::string key = kind + "-" + value;
            auto* item = row_box(*user, "knowledge-exclusion-" + key);
            item->role = ui::Role::list_item;
            // An item exclusion is "<source>:<id>": a file shows its path, other Sources their name and the id.
            std::u16string shown = u16(value);
            if (const size_t colon = value.find(':'); kind == "item" && colon != std::string::npos) {
                const std::string source = value.substr(0, colon), id = value.substr(colon + 1);
                const std::string source_key = "settings.knowledge.source." + source;
                shown = source == "files" || !shell_.ui().strings.has(source_key) ? u16(id) : shell_.t(source_key) + u": " + u16(id);
            }
            const std::u16string what = shell_.t("settings.knowledge.excluded_" + kind, {{"name", shown}});
            item->name = what;
            item->emplace<ui::Label>(shell_.ui(), what, TextStyle::body, Paint::text, false)->role = ui::Role::none;
            auto* rm = button(*item, "knowledge-exclusion-remove-" + key, shell_.t("settings.mail.remove"), [this, kind, value] {
                act("knowledge", "knowledge.exclusion.remove", {{"kind", kind}, {"value", value}}, [this](const json&) { load_knowledge(); });
            }, ui::Button::Kind::subtle);
            rm->name = shell_.t("settings.mail.remove_named", {{"name", what}});
            rm->enabled = busy_.empty();
        }
    if (!any_ex) text(*user, shell_.t("settings.mail.no_exclusions"), Paint::text_secondary, "settings-knowledge-no-exclusions");
    kept_field(shell_, c, "settings-knowledge-exclusion", shell_.t("settings.knowledge.exclusion_value"), knowledge_exclusion_);
    auto* adds = row_box(c);
    for (const char* kind : {"path", "type"}) {
        button(*adds, std::string("settings-knowledge-exclude-") + kind, shell_.t(std::string("settings.knowledge.exclude_") + kind),
               [this, kind = std::string(kind)] {
                   if (blank(knowledge_exclusion_)) return;
                   act("knowledge", "knowledge.exclusion.add", {{"kind", kind}, {"value", ui::to_utf8(knowledge_exclusion_)}}, [this](const json&) {
                       knowledge_exclusion_.clear();
                       load_knowledge();
                   });
               })->enabled = busy_.empty();
    }
    // Individual files, picked: excluded before they are indexed, and removed from the store if they were.
    button(*adds, "settings-knowledge-exclude-file", shell_.t("settings.knowledge.exclude_file"), [this] {
        std::vector<std::string> paths;
        for (const auto& p : shell_.pick_files()) paths.push_back(narrow(p));
        exclude_knowledge_files(std::move(paths));
    })->enabled = busy_.empty();

    // ---- Status: backlog, budget, embedding model ----
    c.emplace<ui::Label>(shell_.ui(), shell_.t("settings.knowledge.status"), TextStyle::body_strong)->role = ui::Role::heading;
    const json* backlog = jobj(knowledge_, "backlog");
    row(c, "settings-knowledge-backlog", shell_.t("settings.knowledge.backlog"),
        shell_.t("settings.knowledge.backlog_value", {{"embed", num(backlog ? jint(*backlog, "embed") : 0)},
                                                      {"extract", num(backlog ? jint(*backlog, "extract") : 0)}}));
    if (const json* b = jobj(knowledge_, "budget")) {
        row(c, "settings-knowledge-budget", shell_.t("settings.knowledge.budget"),
            shell_.t("settings.knowledge.budget_value", {{"tokens", num(jint(*b, "embed_tokens_used"))},
                                                         {"token_limit", num(jint(*b, "embed_tokens_limit"))},
                                                         {"calls", num(jint(*b, "extract_calls_used"))},
                                                         {"call_limit", num(jint(*b, "extract_calls_limit"))}}));
        if (jbool(*b, "embed_exhausted") || jbool(*b, "extract_exhausted"))
            text(c, shell_.t("settings.knowledge.budget_reached"), Paint::warning, "settings-knowledge-budget-reached");
    }
    if (jstr(knowledge_, "extraction") == "off") text(c, shell_.t("settings.knowledge.extraction_off"), Paint::text_secondary, "settings-knowledge-extraction-off");
    if (const json* em = jobj(knowledge_, "embedding")) {
        const std::string model = jstr(*em, "model");
        row(c, "settings-knowledge-embedding", shell_.t("settings.knowledge.embedding"),
            model.empty() ? shell_.t("settings.knowledge.no_embeddings") : u16(model + " (" + std::to_string(jint(*em, "dimension")) + ")"));
        if (jbool(*em, "reindexing")) text(c, shell_.t("settings.knowledge.reembedding"), Paint::text_secondary, "settings-knowledge-reembedding");
    }
    if (const json* errs = jobj(knowledge_, "errors"))
        for (const char* k : {"embed", "extract"})
            if (const std::string e = jstr(*errs, k); !e.empty())
                text(c, shell_.t(std::string("settings.knowledge.error_") + k, {{"reason", u16(e)}}), Paint::warning, std::string("settings-knowledge-error-") + k);

    // ---- Forget an item ----
    c.emplace<ui::Label>(shell_.ui(), shell_.t("settings.knowledge.items"), TextStyle::body_strong)->role = ui::Role::heading;
    kept_field(shell_, c, "settings-knowledge-item-query", shell_.t("settings.knowledge.item_query"), knowledge_item_query_);
    button(c, "settings-knowledge-find-items", shell_.t("settings.knowledge.find"), [this] { find_knowledge(false); })->enabled = busy_.empty();
    if (knowledge_items_.is_array()) {
        auto* items = c.emplace<Node>("settings-knowledge-items");
        items->role = ui::Role::list;
        items->name = shell_.t("settings.knowledge.items");
        items->gap = 4;
        if (knowledge_items_.empty()) text(*items, shell_.t("settings.knowledge.nothing_found"), Paint::text_secondary, "settings-knowledge-no-items");
        for (const auto& it : knowledge_items_) {
            const long long id = jint(it, "id");
            const std::u16string what = u16(jstr(it, "title")) + u" (" + shell_.t("settings.knowledge.source." + jstr(it, "source")) + u")";
            auto* item = row_box(*items, "knowledge-item-" + std::to_string(id));
            item->role = ui::Role::list_item;
            item->name = what;
            item->emplace<ui::Label>(shell_.ui(), what, TextStyle::body, Paint::text, false)->role = ui::Role::none;
            auto* f = button(*item, "knowledge-item-forget-" + std::to_string(id), shell_.t("settings.knowledge.forget"), [this, id, what] {
                ask_forget("knowledge", what, "knowledge.item.forget", {{"item_id", id}}, [this] { find_knowledge(false); });
            }, ui::Button::Kind::subtle);
            f->name = shell_.t("settings.knowledge.forget_named", {{"name", what}});
            f->enabled = busy_.empty();
        }
    }

    // ---- Entities: forget, merge and split ----
    c.emplace<ui::Label>(shell_.ui(), shell_.t("settings.knowledge.entities"), TextStyle::body_strong)->role = ui::Role::heading;
    text(c, shell_.t("settings.knowledge.entities_body"), Paint::text_secondary);
    kept_field(shell_, c, "settings-knowledge-entity-query", shell_.t("settings.knowledge.entity_query"), knowledge_entity_query_);
    button(c, "settings-knowledge-find-entities", shell_.t("settings.knowledge.find"), [this] { find_knowledge(true); })->enabled = busy_.empty();
    if (knowledge_merge_from_)
        text(c, shell_.t("settings.knowledge.merging", {{"name", knowledge_merge_name_}}), Paint::text_secondary, "settings-knowledge-merging");
    if (knowledge_entities_.is_array()) {
        auto* list = c.emplace<Node>("settings-knowledge-entities");
        list->role = ui::Role::list;
        list->name = shell_.t("settings.knowledge.entities");
        list->gap = 4;
        if (knowledge_entities_.empty()) text(*list, shell_.t("settings.knowledge.nothing_found"), Paint::text_secondary, "settings-knowledge-no-entities");
        for (const auto& e : knowledge_entities_) {
            const long long id = jint(e, "id");
            const std::string sid = std::to_string(id);
            std::u16string what = u16(jstr(e, "name")) + u" (" + u16(jstr(e, "type")) + u")";
            if (const std::string mail = jstr(e, "email"); !mail.empty()) what += u" <" + u16(mail) + u">";
            auto* item = row_box(*list, "knowledge-entity-" + sid);
            item->role = ui::Role::list_item;
            item->name = what;
            item->emplace<ui::Label>(shell_.ui(), what, TextStyle::body, Paint::text, false)->role = ui::Role::none;
            button(*item, "knowledge-entity-open-" + sid, shell_.t("settings.knowledge.details"), [this, id] { open_entity(id); }, ui::Button::Kind::subtle)
                ->enabled = busy_.empty();
            if (!knowledge_merge_from_) {
                button(*item, "knowledge-entity-merge-" + sid, shell_.t("settings.knowledge.merge"), [this, id, what] {
                    knowledge_merge_from_ = id;
                    knowledge_merge_name_ = what;
                    rebuild();
                }, ui::Button::Kind::subtle)->enabled = busy_.empty();
            } else if (knowledge_merge_from_ != id) {
                auto* into = button(*item, "knowledge-entity-merge-into-" + sid, shell_.t("settings.knowledge.merge_here"), [this, id] {
                    const long long from = knowledge_merge_from_;
                    knowledge_merge_from_ = 0;
                    act("knowledge", "knowledge.entity.merge", {{"from", from}, {"into", id}}, [this, id](const json&) {
                        shell_.announce(shell_.t("settings.knowledge.merged"));
                        find_knowledge(true);
                        open_entity(id);
                    });
                });
                into->name = shell_.t("settings.knowledge.merge_into", {{"from", knowledge_merge_name_}, {"into", what}});
                into->enabled = busy_.empty();
            } else {
                button(*item, "knowledge-entity-merge-cancel", shell_.t("settings.cancel"), [this] {
                    knowledge_merge_from_ = 0;
                    rebuild();
                }, ui::Button::Kind::subtle);
            }
            auto* f = button(*item, "knowledge-entity-forget-" + sid, shell_.t("settings.knowledge.forget"), [this, id, what] {
                ask_forget("knowledge", what, "knowledge.entity.forget", {{"id", id}}, [this, id] {
                    if (knowledge_entity_.is_object() && jint(knowledge_entity_, "id") == id) knowledge_entity_ = nullptr;
                    find_knowledge(true);
                });
            }, ui::Button::Kind::subtle);
            f->name = shell_.t("settings.knowledge.forget_named", {{"name", what}});
            f->enabled = busy_.empty();
        }
    }
    // The chosen entity: what is known, and its aliases to split off.
    if (knowledge_entity_.is_object()) {
        const long long id = jint(knowledge_entity_, "id");
        auto* box = c.emplace<Node>("settings-knowledge-entity");
        box->gap = 4;
        box->emplace<ui::Label>(shell_.ui(), u16(jstr(knowledge_entity_, "name")), TextStyle::body_strong)->role = ui::Role::heading;
        for (const char* k : {"email", "department", "title", "manager"})
            if (const std::string v = jstr(knowledge_entity_, k); !v.empty())
                row(*box, std::string("knowledge-entity-") + k, shell_.t(std::string("settings.knowledge.field_") + k), u16(v));
        row(*box, "knowledge-entity-mentions", shell_.t("settings.knowledge.mentions"), num(jint(knowledge_entity_, "mentions")));
        if (const json* rels = jobj(knowledge_entity_, "relations"); rels && rels->is_array())
            for (const auto& r : *rels)
                text(*box, u16(jstr(r, "subject") + " \xE2\x80\x94 " + jstr(r, "predicate") + " \xE2\x80\x94 " + jstr(r, "object")), Paint::text_secondary);
        if (const json* facts = jobj(knowledge_entity_, "facts"); facts && facts->is_array())
            for (const auto& fct : *facts) text(*box, shell_.t("settings.knowledge.fact", {{"text", u16(jstr(fct, "text"))}}), Paint::text_secondary);
        if (const json* aliases = jobj(knowledge_entity_, "aliases"); aliases && aliases->is_array() && aliases->size() > 1)
            for (const auto& a : *aliases) {
                if (!a.is_string()) continue;
                const std::string alias = a.get<std::string>();
                auto* line = row_box(*box, "knowledge-alias-" + alias);
                line->role = ui::Role::list_item;
                line->name = u16(alias);
                line->emplace<ui::Label>(shell_.ui(), u16(alias), TextStyle::body, Paint::text, false)->role = ui::Role::none;
                auto* sp = button(*line, "knowledge-alias-split-" + alias, shell_.t("settings.knowledge.split"), [this, id, alias] {
                    act("knowledge", "knowledge.entity.split", {{"id", id}, {"alias", alias}}, [this, id](const json&) {
                        shell_.announce(shell_.t("settings.knowledge.split_done"));
                        find_knowledge(true);
                        open_entity(id);
                    });
                }, ui::Button::Kind::subtle);
                sp->name = shell_.t("settings.knowledge.split_named", {{"name", u16(alias)}});
                sp->enabled = busy_.empty();
            }
    }
    error_line(c, "knowledge");
}

}  // namespace wb::shell
