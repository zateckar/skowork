// The Skill editor page (client/skills Skill Authoring, Save Thread As Skill, Skill Test Run, Skill Publishing;
// add-skills design D10). The host does the work (skills.source, .validate, .save, .delete, .draft, .keep, .test,
// .files.*, .publish*); the editor keeps the text and shows the results.
#include <algorithm>

#include "platform/win.h"
#include "shell/shell.h"
#include "ui/text16.h"
#include "ui/textfield.h"

namespace wb::shell {

using ui::Node;
using ui::Paint;
using ui::Sizing;
using ui::TextStyle;

namespace {

constexpr const char* kFolders[] = {"scripts", "references", "assets"};

std::u16string u16(const std::string& s) { return ui::to_utf16(s); }

std::vector<std::string> split_list(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (const char c : s + ",") {
        if (c == ',' || c == ';' || c == ' ' || c == '\n' || c == '\t') {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    return out;
}

}  // namespace

SkillEditor::SkillEditor(Shell& shell, const Route& route)
    : Node("skill-editor"), shell_(shell), key_(route.hash), original_(route.skill), draft_thread_(route.thread_id) {
    gap = 12;
    role = ui::Role::group;
    name = shell_.t("skill_editor.title");
    status_ = emplace<ui::Label>(shell_.ui(), u"", TextStyle::body, Paint::text_secondary, true, "skill-editor-status");
    replaced_ = emplace<Node>("skill-editor-replaced");
    replaced_->gap = 4;
    replaced_->role = ui::Role::list;
    replaced_->name = shell_.t("skill_editor.replaced_title");
    replaced_->visible = false;

    ui::TextField::Options fm;
    fm.multiline = true;
    fm.min_lines = 5;
    fm.max_lines = 14;
    frontmatter_ = emplace<ui::TextField>(shell_.ui(), "skill-editor-frontmatter", shell_.t("skill_editor.frontmatter"), fm);
    frontmatter_->on_change = [this] { validate(); };
    ui::TextField::Options bo;
    bo.multiline = true;
    bo.min_lines = 10;
    bo.max_lines = 30;
    body_ = emplace<ui::TextField>(shell_.ui(), "skill-editor-body", shell_.t("skill_editor.body"), bo);
    body_->on_change = [this] { validate(); };
    validation_ = emplace<ui::Label>(shell_.ui(), u"", TextStyle::caption, Paint::text_secondary, true, "skill-editor-validation");

    auto* actions = emplace<Node>("skill-editor-actions");
    actions->axis = ui::Axis::row;
    actions->gap = 8;
    save_ = actions->emplace<ui::Button>(shell_.ui(), "skill-editor-save", shell_.t("skill_editor.save"), ui::Button::Kind::primary);
    save_->width = Sizing::fit;
    save_->on_click = [this] { save(); };
    auto* back = actions->emplace<ui::Button>(shell_.ui(), "skill-editor-back", shell_.t("skill_editor.back"));
    back->width = Sizing::fit;
    back->on_click = [this] { shell_.navigate("#/settings"); };
    delete_ = actions->emplace<ui::Button>(shell_.ui(), "skill-editor-delete", shell_.t("skill_editor.delete"), ui::Button::Kind::subtle);
    delete_->width = Sizing::fit;
    delete_->on_click = [this] { ask_delete(); };
    delete_->visible = !original_.empty();

    // Skill Test Run: a sample input, run in a labelled test Thread with the editor's current text.
    auto* test = emplace<Node>("skill-editor-test-row");
    test->axis = ui::Axis::row;
    test->gap = 8;
    test_input_ = test->emplace<ui::TextField>(shell_.ui(), "skill-editor-test-input", shell_.t("skill_editor.test_input"));
    test_input_->set_placeholder(shell_.t("skill_editor.test_placeholder"));
    test_ = test->emplace<ui::Button>(shell_.ui(), "skill-editor-test", shell_.t("skill_editor.test"));
    test_->width = Sizing::fit;
    test_->on_click = [this] { this->test(); };

    // Files of a saved Skill.
    files_section_ = emplace<Node>("skill-editor-files-section");
    files_section_->gap = 6;
    files_section_->role = ui::Role::group;
    files_section_->name = shell_.t("skill_editor.files_title");
    files_section_->emplace<ui::Label>(shell_.ui(), shell_.t("skill_editor.files_title"), TextStyle::body_strong)->role = ui::Role::heading;
    files_ = files_section_->emplace<Node>("skill-editor-files");
    files_->gap = 4;
    files_->role = ui::Role::list;
    files_->name = shell_.t("skill_editor.files_title");
    auto* add = files_section_->emplace<Node>();
    add->axis = ui::Axis::row;
    add->gap = 8;
    folder_ = add->emplace<ui::Button>(shell_.ui(), "skill-files-folder", u"");
    folder_->width = Sizing::fit;
    folder_->on_click = [this] {
        std::vector<std::pair<std::string, std::u16string>> options;
        for (const char* f : kFolders) options.emplace_back(f, u16(f));
        menu(folder_, "skill-files-folder", options, [this](const std::string& v) {
            folder_choice_ = v;
            rebuild_files();
        });
    };
    auto* pick = add->emplace<ui::Button>(shell_.ui(), "skill-files-add", shell_.t("skill_editor.add_files"));
    pick->width = Sizing::fit;
    pick->on_click = [this] { add_files(); };

    // Publishing a saved Skill.
    publish_section_ = emplace<Node>("skill-editor-publish-section");
    publish_section_->gap = 6;
    publish_section_->role = ui::Role::group;
    publish_section_->name = shell_.t("skill_editor.publish_title");
    publish_section_->emplace<ui::Label>(shell_.ui(), shell_.t("skill_editor.publish_title"), TextStyle::body_strong)->role = ui::Role::heading;
    auto* prow = publish_section_->emplace<Node>();
    prow->axis = ui::Axis::row;
    prow->gap = 8;
    scope_ = prow->emplace<ui::Button>(shell_.ui(), "skill-publish-scope", u"");
    scope_->width = Sizing::fit;
    scope_->on_click = [this] {
        std::vector<std::pair<std::string, std::u16string>> options;
        for (const auto& s : scopes_) options.emplace_back(s, shell_.t("skill_editor.scopes." + s));
        menu(scope_, "skill-publish-scope", options, [this](const std::string& v) {
            scope_choice_ = v;
            rebuild_publishing();
        });
    };
    audience_ = prow->emplace<ui::TextField>(shell_.ui(), "skill-publish-audience", shell_.t("skill_editor.users"));
    auto* go = prow->emplace<ui::Button>(shell_.ui(), "skill-publish", shell_.t("skill_editor.publish"));
    go->width = Sizing::fit;
    go->on_click = [this] { publish(); };
    audience_hint_ = publish_section_->emplace<ui::Label>(shell_.ui(), u"", TextStyle::caption, Paint::text_secondary, true, "skill-publish-hint");
    publish_result_ = publish_section_->emplace<ui::Label>(shell_.ui(), u"", TextStyle::body, Paint::text, true, "skill-publish-result");
    publish_result_->visible = false;
    published_ = publish_section_->emplace<Node>("skill-published");
    published_->gap = 2;
    published_->role = ui::Role::list;
    published_->name = shell_.t("skill_editor.published_title");

    rebuild_files();
    rebuild_publishing();
    update_buttons();
    load();
}

void SkillEditor::show_status(const std::u16string& text, Paint color) {
    status_->set_text(text);
    status_->color = color;
    status_->visible = !text.empty();
    relayout();
    invalidate();
}

json SkillEditor::source() const {
    return {{"frontmatter", ui::to_utf8(frontmatter_->text())}, {"body", ui::to_utf8(body_->text())}};
}

void SkillEditor::set_source(const json& s) {
    frontmatter_->set_text(u16(jstr(s, "frontmatter")));
    body_->set_text(u16(jstr(s, "body")));
    validate();
}

void SkillEditor::load() {
    std::weak_ptr<int> alive = alive_;
    if (!original_.empty()) {
        show_status(shell_.t("common.loading"));
        shell_.call("skills.source", {{"name", original_}}, [this, alive](Result<json> r) {
            if (!alive.lock()) return;
            if (!r) return show_status(shell_.t("common.error_prefix") + u" " + u16(r.error().message), Paint::error);
            loaded_ = true;
            show_status({});
            files_list_ = r->contains("files") ? (*r)["files"] : json::array();
            set_source(*r);
            rebuild_files();
        });
        load_publishing();
    } else if (!draft_thread_.empty()) {
        // Save Thread As Skill: the host drafts from the Thread; the user reviews before saving.
        show_status(shell_.t("skill_editor.drafting"));
        busy_ = true;
        update_buttons();
        shell_.call("skills.draft", {{"thread_id", draft_thread_}}, [this, alive](Result<json> r) {
            if (!alive.lock()) return;
            busy_ = false;
            if (!r) {
                show_status(shell_.t("skill_editor.draft_failed") + u" " + u16(r.error().message), Paint::error);
                set_source({{"frontmatter", "name: my-skill\ndescription: \nversion: 0.1.0\n"}, {"body", ""}});
                return;
            }
            loaded_ = true;
            show_status(shell_.t("skill_editor.drafted"));
            replaced_list_ = r->contains("replaced") ? (*r)["replaced"] : json::array();
            rebuild_replaced();
            set_source(*r);
        });
    } else {
        loaded_ = true;
        show_status({});
        set_source({{"frontmatter", "name: my-skill\ndescription: \nversion: 0.1.0\n"}, {"body", ""}});
    }
}

void SkillEditor::validate() {
    const uint64_t gen = ++generation_;
    std::weak_ptr<int> alive = alive_;
    shell_.call("skills.validate", source(), [this, alive, gen](Result<json> r) {
        if (!alive.lock() || gen != generation_) return;
        valid_ = r && jbool(*r, "valid");
        if (!r) {
            validation_->set_text(shell_.t("common.error_prefix") + u" " + u16(r.error().message));
            validation_->color = Paint::error;
        } else if (valid_) {
            validation_->set_text(shell_.t("skill_editor.valid", {{"name", u16(jstr(*r, "name"))}, {"version", u16(jstr(*r, "version"))}}));
            validation_->color = Paint::success;
        } else {
            const json* e = jobj(*r, "error");
            validation_->set_text(shell_.t("settings.skills.error", {{"field", u16(e ? jstr(*e, "field") : "")}, {"message", u16(e ? jstr(*e, "message") : "")}}));
            validation_->color = Paint::error;
        }
        update_buttons();
        relayout();
        invalidate();
    });
}

void SkillEditor::update_buttons() {
    save_->enabled = valid_ && !busy_;
    test_->enabled = valid_ && !busy_;
    delete_->enabled = !busy_;
    files_section_->visible = !original_.empty();
    publish_section_->visible = !original_.empty();
    invalidate();
}

void SkillEditor::save() {
    if (!valid_ || busy_) return;
    busy_ = true;
    update_buttons();
    json p = source();
    p["original"] = original_;
    std::weak_ptr<int> alive = alive_;
    shell_.call("skills.save", p, [this, alive](Result<json> r) {
        if (!alive.lock()) return;
        busy_ = false;
        update_buttons();
        if (!r) return show_status(shell_.t("common.error_prefix") + u" " + u16(r.error().message), Paint::error);
        const std::string saved = jstr(*r, "name");
        shell_.announce(shell_.t("skill_editor.saved", {{"name", u16(saved)}}));
        if (saved != original_) {
            shell_.navigate("#/skills/edit/" + saved);  // the editor of the saved Skill, with its files and publishing
            return;
        }
        show_status(shell_.t("skill_editor.saved", {{"name", u16(saved)}}), Paint::success);
        files_list_ = r->contains("files") ? (*r)["files"] : json::array();
        rebuild_files();
    });
}

void SkillEditor::ask_delete() {
    auto d = std::make_unique<ui::Dialog>(shell_.ui(), "skill-delete-dialog", shell_.t("skill_editor.delete_title", {{"name", u16(original_)}}));
    d->body().emplace<ui::Label>(shell_.ui(), shell_.t("skill_editor.delete_body"));
    ui::Dialog* dp = d.get();
    std::weak_ptr<int> alive = alive_;
    d->add_button("skill-delete-confirm", shell_.t("skill_editor.delete"), ui::Button::Kind::primary, [this, alive, dp] {
        dp->close();
        shell_.call("skills.delete", {{"name", original_}}, [this, alive](Result<json> r) {
            if (!alive.lock()) return;
            if (!r) return show_status(shell_.t("common.error_prefix") + u" " + u16(r.error().message), Paint::error);
            shell_.announce(shell_.t("skill_editor.deleted", {{"name", u16(original_)}}));
            shell_.navigate("#/settings");
        });
    });
    ui::Button* cancel = d->add_button("skill-delete-cancel", shell_.t("settings.cancel"), ui::Button::Kind::standard, [dp] { dp->close(); });
    shell_.window().modal().show(std::move(d));
    shell_.window().tree().set_focus(cancel);  // a destructive action is never the default
    shell_.window().render();
}

void SkillEditor::test() {
    if (!valid_ || busy_) return;
    busy_ = true;
    update_buttons();
    json p = source();
    p["input"] = ui::to_utf8(test_input_->text());
    std::weak_ptr<int> alive = alive_;
    shell_.call("skills.test", p, [this, alive](Result<json> r) {
        if (!alive.lock()) return;
        busy_ = false;
        update_buttons();
        if (!r) return show_status(shell_.t("common.error_prefix") + u" " + u16(r.error().message), Paint::error);
        shell_.navigate(thread_hash(jstr(*r, "thread_id")));
    });
}

void SkillEditor::rebuild_replaced() {
    replaced_->clear_children();
    replaced_->visible = !replaced_list_.empty();
    if (replaced_list_.empty()) return;
    replaced_->emplace<ui::Label>(shell_.ui(), shell_.t("skill_editor.replaced_title"), TextStyle::body_strong)->role = ui::Role::heading;
    replaced_->emplace<ui::Label>(shell_.ui(), shell_.t("skill_editor.replaced_body"), TextStyle::caption, Paint::text_secondary, true);
    for (size_t i = 0; i < replaced_list_.size(); ++i) {
        const json& r = replaced_list_[i];
        auto* item = replaced_->emplace<Node>("skill-replaced-" + std::to_string(i));
        item->axis = ui::Axis::row;
        item->gap = 8;
        item->role = ui::Role::list_item;
        const std::u16string line = shell_.t("skill_editor.replaced." + jstr(r, "kind"),
                                             {{"value", u16(jstr(r, "original"))}, {"parameter", u16("{{" + jstr(r, "parameter") + "}}")}});
        item->name = line;
        item->emplace<ui::Label>(shell_.ui(), line, TextStyle::body, Paint::text, true)->role = ui::Role::none;
        auto* k = item->emplace<ui::Button>(shell_.ui(), "skill-keep-" + std::to_string(i), shell_.t("skill_editor.keep"), ui::Button::Kind::subtle);
        k->width = Sizing::fit;
        k->name = shell_.t("skill_editor.keep_named", {{"value", u16(jstr(r, "original"))}});
        k->on_click = [this, i] { keep(i); };
    }
    relayout();
    invalidate();
}

void SkillEditor::keep(size_t index) {
    if (index >= replaced_list_.size()) return;
    json p = source();
    p["replacement"] = replaced_list_[index];
    std::weak_ptr<int> alive = alive_;
    shell_.call("skills.keep", p, [this, alive, index](Result<json> r) {
        if (!alive.lock() || !r || index >= replaced_list_.size()) return;
        shell_.announce(shell_.t("skill_editor.kept", {{"value", u16(jstr(replaced_list_[index], "original"))}}));
        replaced_list_.erase(replaced_list_.begin() + static_cast<std::ptrdiff_t>(index));
        rebuild_replaced();
        set_source(*r);
    });
}

void SkillEditor::rebuild_files() {
    folder_->set_label(shell_.t("skill_editor.add_to", {{"folder", u16(folder_choice_)}}) + u"  ▾");
    files_->clear_children();
    if (files_list_.empty()) {
        files_->emplace<ui::Label>(shell_.ui(), shell_.t("skill_editor.no_files"), TextStyle::caption, Paint::text_secondary, true, "skill-files-empty");
    }
    for (size_t i = 0; i < files_list_.size(); ++i) {
        const std::string path = jstr(files_list_[i], "path");
        auto* item = files_->emplace<Node>("skill-file-" + std::to_string(i));
        item->axis = ui::Axis::row;
        item->gap = 8;
        item->role = ui::Role::list_item;
        item->name = u16(path);
        item->emplace<ui::Label>(shell_.ui(), u16(path), TextStyle::body, Paint::text, false)->role = ui::Role::none;
        auto* rm = item->emplace<ui::Button>(shell_.ui(), "skill-file-remove-" + std::to_string(i), shell_.t("skill_editor.remove_file"),
                                             ui::Button::Kind::subtle);
        rm->width = Sizing::fit;
        rm->name = shell_.t("skill_editor.remove_named", {{"file", u16(path)}});
        rm->on_click = [this, path] { remove_file(path); };
    }
    relayout();
    invalidate();
}

void SkillEditor::add_files() {
    const auto picked = shell_.pick_files();
    if (picked.empty() || original_.empty()) return;
    json paths = json::array();
    for (const auto& p : picked) paths.push_back(narrow(p));
    std::weak_ptr<int> alive = alive_;
    shell_.call("skills.files.add", {{"name", original_}, {"folder", folder_choice_}, {"paths", paths}}, [this, alive](Result<json> r) {
        if (!alive.lock()) return;
        if (!r) return show_status(shell_.t("common.error_prefix") + u" " + u16(r.error().message), Paint::error);
        files_list_ = (*r)["files"];
        rebuild_files();
    });
}

void SkillEditor::remove_file(const std::string& path) {
    std::weak_ptr<int> alive = alive_;
    shell_.call("skills.files.remove", {{"name", original_}, {"path", path}}, [this, alive](Result<json> r) {
        if (!alive.lock()) return;
        if (!r) return show_status(shell_.t("common.error_prefix") + u" " + u16(r.error().message), Paint::error);
        files_list_ = (*r)["files"];
        rebuild_files();
    });
}

void SkillEditor::load_publishing() {
    std::weak_ptr<int> alive = alive_;
    shell_.call("skills.publish_options", json::object(), [this, alive](Result<json> r) {
        if (!alive.lock() || !r) return;
        scopes_.clear();
        groups_.clear();
        for (const auto& s : (*r)["scopes"])
            if (s.is_string()) scopes_.push_back(s.get<std::string>());
        for (const auto& g : (*r)["groups"])
            if (g.is_string()) groups_.push_back(g.get<std::string>());
        if (std::find(scopes_.begin(), scopes_.end(), scope_choice_) == scopes_.end() && !scopes_.empty()) scope_choice_ = scopes_.front();
        rebuild_publishing();
    });
    shell_.call("skills.published", json::object(), [this, alive](Result<json> r) {
        if (!alive.lock()) return;
        published_list_ = json::array();
        if (r && r->is_array())
            for (const auto& v : *r)
                if (jstr(v, "name") == original_) published_list_.push_back(v);
        rebuild_publishing();
    });
}

void SkillEditor::rebuild_publishing() {
    scope_->set_label(shell_.t("skill_editor.scopes." + scope_choice_) + u"  ▾");
    scope_->name = shell_.t("skill_editor.scope") + u": " + shell_.t("skill_editor.scopes." + scope_choice_);
    audience_->visible = scope_choice_ != "org";
    audience_->name = shell_.t(scope_choice_ == "group" ? "skill_editor.groups" : "skill_editor.users");
    std::string groups;
    for (const auto& g : groups_) groups += (groups.empty() ? "" : ", ") + g;
    audience_hint_->set_text(scope_choice_ == "group"   ? shell_.t("skill_editor.groups_hint", {{"groups", u16(groups)}})
                             : scope_choice_ == "org" ? shell_.t("skill_editor.org_hint")
                                                       : shell_.t("skill_editor.users_hint"));
    published_->clear_children();
    for (size_t i = 0; i < published_list_.size(); ++i) {
        const json& v = published_list_[i];
        const std::string state = jstr(v, "state");
        std::u16string line = shell_.t("skill_editor.published_line", {{"version", u16(jstr(v, "version"))},
                                                                       {"scope", shell_.t("skill_editor.scopes." + jstr(v, "scope"))},
                                                                       {"state", shell_.t("skill_editor.states." + state)}});
        if (const std::string c = jstr(v, "review_comment"); !c.empty()) line += u" — " + u16(c);
        auto* row = published_->emplace<Node>();
        row->axis = ui::Axis::row;
        row->gap = 8;
        row->emplace<ui::Label>(shell_.ui(), line, TextStyle::caption, Paint::text_secondary, true, "skill-published-" + std::to_string(i))
            ->role = ui::Role::list_item;
        // Author Withdrawal (control-plane/skill-registry): a published or in-review version can be withdrawn.
        if (state == "published" || state == "in-review") {
            const std::string version = jstr(v, "version");
            auto* b = row->emplace<ui::Button>(shell_.ui(), "skill-withdraw-" + std::to_string(i), shell_.t("skill_editor.withdraw"),
                                               ui::Button::Kind::subtle);
            b->width = Sizing::fit;
            b->name = shell_.t("skill_editor.withdraw_version", {{"version", u16(version)}});
            b->on_click = [this, version] { ask_withdraw(version); };
        }
    }
    relayout();
    invalidate();
}

void SkillEditor::publish() {
    if (busy_ || original_.empty()) return;
    json p = {{"name", original_}, {"scope", scope_choice_}};
    const auto list = split_list(ui::to_utf8(audience_->text()));
    if (scope_choice_ == "shared") p["users"] = list;
    if (scope_choice_ == "group") p["groups"] = list;
    busy_ = true;
    update_buttons();
    std::weak_ptr<int> alive = alive_;
    shell_.call("skills.publish", p, [this, alive](Result<json> r) {
        if (!alive.lock()) return;
        busy_ = false;
        update_buttons();
        publish_result_->visible = true;
        if (!r) {
            publish_result_->set_text(shell_.t("skill_editor.publish_failed", {{"message", u16(r.error().message)}}));
            publish_result_->color = Paint::error;
        } else {
            const bool review = jstr(*r, "state") == "in-review";
            publish_result_->set_text(shell_.t(review ? "skill_editor.in_review" : "skill_editor.published",
                                               {{"version", u16(jstr(*r, "version"))}, {"scope", shell_.t("skill_editor.scopes." + jstr(*r, "scope"))}}));
            publish_result_->color = Paint::text;
            load_publishing();
        }
        shell_.announce(publish_result_->text());
        relayout();
        invalidate();
    });
}

void SkillEditor::ask_withdraw(const std::string& version) {
    auto d = std::make_unique<ui::Dialog>(shell_.ui(), "skill-withdraw-dialog",
                                          shell_.t("skill_editor.withdraw_title", {{"name", u16(original_)}, {"version", u16(version)}}));
    d->body().emplace<ui::Label>(shell_.ui(), shell_.t("skill_editor.withdraw_body"));
    ui::Dialog* dp = d.get();
    std::weak_ptr<int> alive = alive_;
    d->add_button("skill-withdraw-confirm", shell_.t("skill_editor.withdraw"), ui::Button::Kind::primary, [this, dp, alive, version] {
        dp->close();
        if (!alive.lock()) return;
        shell_.call("skills.withdraw", {{"name", original_}, {"version", version}}, [this, alive, version](Result<json> r) {
            if (!alive.lock()) return;
            publish_result_->visible = true;
            if (!r) {
                publish_result_->set_text(shell_.t("skill_editor.withdraw_failed", {{"message", u16(r.error().message)}}));
                publish_result_->color = Paint::error;
            } else {
                publish_result_->set_text(shell_.t("skill_editor.withdrawn", {{"version", u16(version)}}));
                publish_result_->color = Paint::text;
                load_publishing();
            }
            shell_.announce(publish_result_->text());
            relayout();
            invalidate();
        });
    });
    ui::Button* cancel = d->add_button("skill-withdraw-cancel", shell_.t("thread.cancel"), ui::Button::Kind::standard, [dp] { dp->close(); });
    shell_.window().modal().show(std::move(d));
    shell_.window().tree().set_focus(cancel);  // a destructive action is never the default
    shell_.window().render();
}

void SkillEditor::menu(ui::Button* anchor, const std::string& id, std::vector<std::pair<std::string, std::u16string>> options,
                       std::function<void(const std::string&)> pick) {
    auto m = std::make_unique<ui::Menu>(id + "-menu", anchor->name);
    for (const auto& [value, text] : options) m->add_item(shell_.ui(), id + "-" + value, text, [pick, value] { pick(value); });
    shell_.window().overlay().show(std::move(m), anchor->frame(), true);
    shell_.window().render();
}

}  // namespace wb::shell
