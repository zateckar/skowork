// The Thread feed (client/work-inbox Streaming Thread Feed, Markdown Rendering, Fork From Message).
#include <algorithm>

#include "shell/shell.h"
#include "platform/crypto.h"
#include "ui/generated_save_queue.h"
#include "ui/generated_surface_view.h"
#include "ui/text16.h"

namespace wb::shell {

using ui::Node;
using ui::Paint;
using ui::Sizing;
using ui::TextStyle;

namespace {

std::string surface_cache_key(const FeedItem& item, size_t index, const ui::GeneratedSurfaceBlock& block) {
    const auto content = std::string_view(item.text).substr(block.begin, block.end - block.begin);
    return item.key + ":surface:" + std::to_string(index) + ":" + hex_encode(sha256(content));
}

ui::GeneratedCitationResolver surface_citation_resolver(Shell& shell, std::weak_ptr<int> alive,
                                                        std::vector<Citation> sources) {
    return [&shell, alive, sources = std::move(sources)](const std::string& id) -> std::optional<ui::GeneratedCitation> {
        if (!alive.lock()) return std::nullopt;
        for (const auto& source : sources) {
            if (std::to_string(source.n) != id) continue;
            const std::string ref = source.ref;
            return ui::GeneratedCitation{ui::to_utf16(source.title.empty() ? ref : source.title), [&shell, alive, ref] {
                if (!alive.lock()) return;
                if (ref.starts_with("skowork://thread/")) shell.navigate(thread_hash(ref.substr(sizeof("skowork://thread/") - 1)));
                else if (ref.starts_with("https://") || ref.starts_with("http://")) {
                    if (shell.ui().open_link) shell.ui().open_link(ui::to_utf16(ref));
                } else {
                    shell.call("knowledge.open", {{"ref", ref}}, [&shell, alive](Result<json> result) {
                        if (alive.lock() && !result)
                            shell.announce(shell.t("feed.citation_unavailable", {{"reason", ui::to_utf16(result.error().message)}}));
                    });
                }
            }};
        }
        return std::nullopt;
    };
}

// Automation ids from item keys ("run_1:12" -> "run_1-12").
std::string id_of(std::string_view prefix, const std::string& key) {
    std::string s(prefix);
    for (char c : key) s += (c == ':' ? '-' : c);
    return s;
}

// A timestamp in the window's language: the time today, the date and time otherwise.
std::u16string format_at(Shell& s, const std::string& iso) {
    const auto ms = iso_millis(iso);
    if (!ms) return {};
    return s.moment(*ms);
}

// The sources an answer lists, as part of its signature: a change to them rebuilds the answer's element.
std::string sources_signature(const std::vector<Citation>& sources) {
    std::string s;
    for (const Citation& c : sources) s += "|" + std::to_string(c.n) + c.title + c.ref;
    return s;
}

std::string signature(const FeedItem& it) {
    std::string s = std::to_string(static_cast<int>(it.type)) + "|";
    switch (it.type) {
    case ItemType::user: return s + it.text;
    case ItemType::assistant:  // live text is updated in place
        return s + (it.streaming ? "live" : it.text) + sources_signature(it.sources) + sources_signature(it.surface_sources);
    case ItemType::reasoning: return s + (it.streaming ? "live" : it.text);
    case ItemType::details: return s + it.text;
    case ItemType::tool:
        s += std::string(it.compact ? "c|" : "") + it.tool + "|" + it.risk + "|" + it.decision + "|" + it.reason + "|" + (it.edited ? "e" : "") + "|" + it.args.dump();
        if (it.result) s += "|" + std::string(it.result->unknown ? "unknown" : it.result->ok ? "ok" : "bad") + "|" + it.result->content;
        if (it.result)
            for (const FileRef& f : it.result->files) s += "|" + f.id;
        return s;
    case ItemType::action: return s + it.action.id + "|" + it.action.resolved_at + "|" + it.action.resolution.dump() + sources_signature(it.surface_sources);
    case ItemType::error: return s + it.error.code;
    case ItemType::stopped: return s + (it.by_user ? "u" : "") + it.reason;
    case ItemType::limit: return s + it.reason + "|" + it.text;
    case ItemType::activity:
        if (it.usage)
            return s + "usage|" + std::to_string(it.usage->prompt) + "|" + std::to_string(it.usage->completion) + "|" +
                   std::to_string(it.usage->reasoning) + "|" + std::to_string(it.usage->calls);
        return s + it.text;
    case ItemType::working: return s + it.text;
    case ItemType::speed:
        if (!it.speed) return s;
        return s + std::to_string(it.speed->tokens_per_s) + (it.speed->approximate ? "~" : "") + "|" +
               std::to_string(it.speed->waiting_ms < 0 ? -1 : it.speed->waiting_ms / 1000);
    }
    return s;
}

// The plain step a tool call stands for, as a string key (Work Details Visibility, design D2).
std::string step_key(const std::string& tool, const std::string& risk) {
    if (tool.starts_with("files.")) return risk == "read" ? "feed.step.reading_files" : "feed.step.writing_files";
    if (tool.starts_with("web.")) return "feed.step.web";
    if (tool.starts_with("browser.")) return "feed.step.browser";
    if (tool.starts_with("computer.")) return "feed.step.computer";
    if (tool.starts_with("shell.")) return "feed.step.commands";
    if (tool.starts_with("mail.") || tool.starts_with("calendar.")) return "feed.step.mail";
    if (tool.starts_with("knowledge.")) return "feed.step.knowledge";
    if (tool.starts_with("image.")) return "feed.step.image";
    if (tool.starts_with("mcp:")) return "feed.step.mcp";
    return "feed.working";
}

// The MCP server of an mcp:<server>/<tool> id.
std::string mcp_server(const std::string& tool) {
    if (!tool.starts_with("mcp:")) return {};
    const auto slash = tool.find('/');
    return tool.substr(4, slash == std::string::npos ? std::string::npos : slash - 4);
}

// A tool result that stays visible with work details hidden: an image it stored (its sources are listed under the
// answer that cites them, Cited Sources In The Feed).
bool produced_image(const FeedItem& it) {
    if (!it.result) return false;
    for (const FileRef& f : it.result->files)
        if (f.image()) return true;
    return false;
}

// A tool result with something to show under its card: an image or the passages it found.
bool produced_something(const FeedItem& it) {
    return produced_image(it) || (it.result && (!it.result->citations.empty() || !it.result->publish_route.empty()));
}

// The header of a message (improve-desktop-look D6): an avatar, who, when, and at the end the icon buttons Copy and,
// on stored messages, Fork from here.
void message_header(Shell& s, Node& msg, const FeedItem& it) {
    auto* head = msg.emplace<Node>();
    head->axis = ui::Axis::row;
    head->gap = 10;
    if (it.type == ItemType::user) {
        // The signed-in user's initials: the messaging name, else the sign-in user name.
        const json* m = jobj(s.status(), "messaging");
        const json* auth = jobj(s.status(), "auth");
        std::string me = m ? jstr(*m, "me") : std::string();
        if (me.empty() && auth) me = jstr(*auth, "user");
        head->emplace<ui::Avatar>(s.ui(), me.empty() ? s.t("feed.you") : ui::to_utf16(me));
    } else {
        head->emplace<ui::Avatar>(s.ui(), u"S", Paint::accent, Paint::accent_text);  // the Skowork mark
    }
    auto* who = head->emplace<ui::Label>(s.ui(), s.t(it.type == ItemType::user ? "feed.you" : "feed.agent"),
                                         TextStyle::body_strong, Paint::text, false);
    who->role = ui::Role::none;
    who->padding.top = 4;
    if (!it.at.empty()) {
        auto* when = head->emplace<ui::Label>(s.ui(), format_at(s, it.at), TextStyle::caption, Paint::text_secondary, false);
        when->role = ui::Role::none;
        when->padding.top = 6;
    }
    auto* spacer = head->emplace<Node>();
    spacer->width = Sizing::fill;
    Shell* sp = &s;
    auto* copy = head->emplace<ui::IconButton>(s.ui(), id_of("copy-", it.key), ui::icon::copy, s.t("feed.copy_message"));
    copy->on_click = [sp, text = it.text] {
        sp->announce(sp->t(sp->copy_text(text) ? "feed.copied" : "question.copy_failed"));
    };
    if (it.seq > 0) {
        auto* fork = head->emplace<ui::IconButton>(s.ui(), id_of("fork-", it.key), ui::icon::fork, s.t("thread.fork_from_here"));
        fork->on_click = [sp, thread = s.open_id(), run = it.run_id, seq = it.seq] { sp->fork(thread, run, seq); };
    }
}

// Text in a monospace box (tool arguments and results).
Node* code_box(Shell& s, Node& parent, const std::string& id, const std::string& text, Paint color = Paint::text) {
    auto* box = parent.emplace<ui::Box>(id, Paint::code_background, 6);
    box->padding = ui::Insets::xy(10, 8);
    auto* t = box->emplace<ui::RichText>(s.ui(), ui::to_utf16(text), std::vector<ui::Span>{}, TextStyle::code, color, id + "-text");
    (void)t;
    return box;
}

// The Approval card and the Question card (client/work-inbox Approval Card, Question Card, Standalone Action Items):
// what is asked, with the decisions while it is open, and what was decided once it is resolved. Edit arguments and
// Respond instead switch the card to a text field with its own confirm and cancel.
class ActionCard : public ui::Box {
public:
    ActionCard(Shell& s, const ActionItem& a, bool generated_surfaces, std::vector<Citation> sources)
        : Box((a.kind == "approval" ? "approval-" : "question-") + a.id, Paint::surface, 8, Paint::border), shell_(s), a_(a), sources_(std::move(sources)) {
        role = ui::Role::group;
        padding = ui::Insets::xy(14, 12);
        gap = 6;
        rebuild();
        if (generated_surfaces && a_.kind == "question" && a_.open()) open_generated_question();
    }
    void draw(ui::Canvas& c) override {
        Box::draw(c);
        // An open card is marked along its left edge, as the web UI's cards are.
        const ui::Rect f = frame();
        c.round({f.x, f.y, 4, f.h}, 2, a_.open() ? (a_.kind == "approval" ? Paint::warning : Paint::focus) : Paint::border);
    }

private:
    enum class Mode { buttons, edit, respond };
    std::string id(const std::string& part) const { return automation_id() + "-" + part; }
    ui::Button* button(Node& row, const std::string& part, const std::u16string& label, bool primary, std::function<void()> f) {
        auto* b = row.emplace<ui::Button>(shell_.ui(), id(part), label, primary ? ui::Button::Kind::primary : ui::Button::Kind::standard);
        b->width = Sizing::fit;
        b->enabled = !busy_;
        b->on_click = std::move(f);
        return b;
    }
    ui::Label* caption(const std::u16string& text) {
        return emplace<ui::Label>(shell_.ui(), text, TextStyle::caption, Paint::text_secondary, true);
    }

    void open_generated_question() {
        const auto* form = jobj(a_.payload, "surface");
        if (!form || !ui::generated_surface_presentation_supported(*form, true, true)) return;
        question_surface_ = *form;
        question_identity_ = {{"thread_id", a_.thread_id}, {"run_id", a_.run_id}, {"action_id", a_.id},
                              {"revision", jstr(a_.payload, "surface_revision")}, {"token", jstr(a_.payload, "surface_token")}};
        std::weak_ptr<int> alive = alive_;
        shell_.call("surfaces.question_open", question_identity_, [this, alive](Result<json> result) {
            if (!alive.lock() || !result) return;
            auto writer = shell_.generated_writer("surfaces.question_save", question_identity_);
            question_writer_ = writer;
            const json saved = writer->latest().value_or(result->value("state", json(nullptr)));
            question_state_ = std::make_shared<ui::GeneratedSurfaceViewState>(
                ui::generated_surface_view_state_from_json(question_surface_, saved));
            rebuild();
        });
    }

    void rebuild() {
        clear_children();
        field_ = nullptr;
        if (a_.kind == "approval") build_approval();
        else build_question();
        if (!error_.empty()) emplace<ui::Label>(shell_.ui(), error_, TextStyle::body, Paint::error, true, id("error"));
        relayout();
        invalidate();
    }

    void build_approval() {
        ui::Ui& u = shell_.ui();
        const json& p = a_.payload;
        const std::u16string tool = ui::to_utf16(jstr(p, "tool"));
        name = shell_.t("approval.title") + u": " + tool;
        auto* h = emplace<ui::Label>(u, name, TextStyle::body_strong, Paint::text, true, id("title"));
        h->role = ui::Role::heading;
        const std::string risk = jstr(p, "risk");
        const std::u16string risk_text = u.strings.has("feed.risk." + risk) ? shell_.t("feed.risk." + risk) : ui::to_utf16(risk);
        emplace<ui::Label>(u, shell_.t("approval.risk") + u": " + risk_text, TextStyle::body, Paint::text, true, id("risk"));
        caption(shell_.t("approval.rationale"));
        const std::string rationale = jstr(p, "rationale");
        emplace<ui::Label>(u, rationale.empty() ? shell_.t("approval.no_rationale") : ui::to_utf16(rationale), TextStyle::body,
                           Paint::text, true, id("rationale"));
        if (const std::string reason = jstr(p, "reason"); !reason.empty()) {
            caption(shell_.t("approval.policy_reason"));
            emplace<ui::Label>(u, ui::to_utf16(reason), TextStyle::body, Paint::text, true, id("reason"));
        }
        caption(shell_.t("approval.args"));
        const json args = p.contains("args") ? p["args"] : json::object();
        if (a_.open() && mode_ == Mode::edit) {
            ui::TextField::Options o;
            o.multiline = true;
            o.min_lines = 4;
            o.max_lines = 14;
            field_ = emplace<ui::TextField>(u, id("args-field"), shell_.t("approval.args_label"), o);
            field_->set_text(draft_.empty() ? ui::to_utf16(args.dump(2)) : draft_);
            field_->on_change = [this] { draft_ = field_->text(); };
        } else {
            code_box(shell_, *this, id("args"), args.dump(2));
        }
        // Provenance: the untrusted inputs read before the request.
        caption(shell_.t("approval.provenance"));
        const json* prov = jobj(p, "untrusted_context");
        if (!prov || !prov->is_array() || prov->empty()) {
            emplace<ui::Label>(u, shell_.t("approval.no_provenance"), TextStyle::body, Paint::text_secondary, true, id("no-provenance"));
        } else {
            auto* list = emplace<Node>(id("provenance"));
            list->role = ui::Role::list;
            list->name = shell_.t("approval.provenance");
            list->gap = 2;
            size_t i = 0;
            for (const json& src : *prov) {
                const std::string kind = jstr(src, "source_kind");
                std::u16string line = u.strings.has("approval.sources." + kind) ? shell_.t("approval.sources." + kind) : ui::to_utf16(kind);
                line += u": " + ui::to_utf16(jstr(src, "source_id"));
                if (const std::string at = jstr(src, "at"); !at.empty()) line += u", " + format_at(shell_, at);
                auto* l = list->emplace<ui::Label>(u, line, TextStyle::body, Paint::text, true, id("source-" + std::to_string(i++)));
                l->role = ui::Role::list_item;
            }
        }
        if (!a_.open()) {
            const std::string kind = jstr(a_.resolution, "kind");
            const std::string key = "approval.resolved." + (u.strings.has("approval.resolved." + kind) ? kind : std::string("cancelled"));
            emplace<ui::Label>(u, shell_.t(key), TextStyle::body_strong, Paint::text, true, id("resolved"));
            return;
        }
        if (mode_ == Mode::respond) {
            ui::TextField::Options o;
            o.multiline = true;
            o.min_lines = 2;
            field_ = emplace<ui::TextField>(u, id("respond-field"), shell_.t("approval.respond_label"), o);
            field_->set_placeholder(shell_.t("approval.respond_label"));
            field_->set_text(draft_);
            field_->on_change = [this] { draft_ = field_->text(); };
        }
        auto* row = emplace<Node>(id("actions"));
        row->axis = ui::Axis::row;
        row->gap = 8;
        row->padding.top = 4;
        if (mode_ == Mode::buttons) {
            button(*row, "approve", shell_.t("approval.approve"), true, [this] { decide("approve"); });
            button(*row, "edit", shell_.t("approval.edit"), false, [this] { set_mode(Mode::edit); });
            button(*row, "reject", shell_.t("approval.reject"), false, [this] { decide("reject"); });
            button(*row, "respond", shell_.t("approval.respond"), false, [this] { set_mode(Mode::respond); });
        } else if (mode_ == Mode::edit) {
            button(*row, "approve-edited", shell_.t("approval.approve_edited"), true, [this] { decide("approve_edited"); });
            button(*row, "cancel", shell_.t("approval.cancel"), false, [this] { set_mode(Mode::buttons); });
        } else {
            button(*row, "send", shell_.t("approval.send"), true, [this] { decide("respond"); });
            button(*row, "cancel", shell_.t("approval.cancel"), false, [this] { set_mode(Mode::buttons); });
        }
    }

    void build_question() {
        ui::Ui& u = shell_.ui();
        const json& p = a_.payload;
        const std::string prompt = jstr(p, "prompt");
        name = shell_.t("question.title") + u": " + ui::to_utf16(prompt);
        auto* h = emplace<ui::Label>(u, shell_.t("question.title"), TextStyle::body_strong, Paint::text, true, id("title"));
        h->role = ui::Role::heading;
        emplace<ui::Label>(u, ui::to_utf16(prompt), TextStyle::body, Paint::text, true, id("prompt"));
        // Copyable text that goes with the Question (client/meeting-capture participant notice).
        if (const std::string copy = jstr(p, "copy"); !copy.empty() && a_.open()) {
            code_box(shell_, *this, id("copy-text"), copy);
            auto* row = emplace<Node>(id("copy-row"));
            row->axis = ui::Axis::row;
            button(*row, "copy", shell_.t("question.copy"), false, [this, copy] {
                if (!shell_.copy_text(copy)) fail(shell_.t("question.copy_failed"));
            });
        }
        if (!a_.open()) {
            const bool answered = a_.resolution.is_object() && a_.resolution.contains("answer");
            emplace<ui::Label>(u, answered ? shell_.t("question.answered", {{"answer", ui::to_utf16(jstr(a_.resolution, "answer"))}})
                                           : shell_.t("question.closed"),
                               TextStyle::body_strong, Paint::text, true, id("resolved"));
            return;
        }
        if (const json* choices = jobj(p, "choices"); choices && choices->is_array() && !choices->empty()) {
            auto* row = emplace<Node>(id("choices"));
            row->axis = ui::Axis::row;
            row->gap = 8;
            size_t i = 0;
            for (const json& c : *choices) {
                const std::string choice = c.is_string() ? c.get<std::string>() : c.dump();
                button(*row, "choice-" + std::to_string(i++), ui::to_utf16(choice), false, [this, choice] { answer(choice); });
            }
        }
        if (question_state_) {
            const auto state = question_state_;
            const auto identity = question_identity_;
            const auto writer = question_writer_;
            std::weak_ptr<int> alive = alive_;
            auto changed = [alive, state, writer] {
                if (!alive.lock()) return;
                writer->save(ui::generated_surface_view_state_to_json(*state));
            };
            ui::GeneratedActionView::Submit submit = [this, alive, state, identity, writer](std::string button_id,
                    std::vector<ui::GeneratedFieldValue> fields, std::function<void(bool)> done) {
                if (!alive.lock() || busy_) return done(false);
                auto params = identity;
                params["button_id"] = std::move(button_id);
                params["draft"] = json::object();
                for (const auto& field : fields) params["draft"][field.id] = field.value;
                params["sections"] = ui::generated_surface_view_state_to_json(*state)["sections"];
                shell_.call("surfaces.answer_question", params, [this, alive, done, writer](Result<json> result) {
                    if (result) writer->abandon();
                    done(result.ok());
                    if (alive.lock() && result) shell_.load_feed();
                });
            };
            emplace<ui::GeneratedSurfaceView>(u, question_surface_, state, changed, submit,
                                               surface_citation_resolver(shell_, alive, sources_));
        }
        if (jbool(p, "free_text")) {
            auto* row = emplace<Node>(id("free"));
            row->axis = ui::Axis::row;
            row->gap = 8;
            field_ = row->emplace<ui::TextField>(u, id("answer"), shell_.t("question.answer_label"));
            field_->set_placeholder(shell_.t("question.answer_label"));
            field_->set_text(draft_);
            field_->on_change = [this] { draft_ = field_->text(); };
            field_->on_key_down = [this](const ui::KeyEvent& e) {
                if (e.key != ui::kKeyEnter || e.shift || e.ctrl || e.alt) return false;
                answer(ui::to_utf8(draft_));
                return true;
            };
            button(*row, "send", shell_.t("question.send"), true, [this] { answer(ui::to_utf8(draft_)); });
        }
    }

    void set_mode(Mode m) {
        mode_ = m;
        error_.clear();
        draft_.clear();
        rebuild();
        // The focus goes to the new field, or back to the first decision.
        if (ui::Tree* tr = Node::tree()) {
            if (field_) tr->set_focus(field_);
            else
                for (const auto& c : children())
                    if (c->automation_id() == id("actions") && !c->children().empty()) tr->set_focus(c->children().front().get());
        }
    }

    void decide(const std::string& decision) {
        json params = {{"action_id", a_.id}, {"decision", decision}};
        if (decision == "approve_edited") {
            const json args = json::parse(ui::to_utf8(draft_.empty() && field_ ? field_->text() : draft_), nullptr, false);
            if (args.is_discarded() || !args.is_object()) return fail(shell_.t("approval.invalid_json"));
            params["args"] = args;
        }
        if (decision == "respond") {
            const std::string text = ui::to_utf8(draft_);
            if (text.find_first_not_of(" \t\r\n") == std::string::npos) return;
            params["text"] = text;
        }
        send("actions.resolve", params);
    }

    void answer(const std::string& value) {
        if (value.find_first_not_of(" \t\r\n") == std::string::npos) return;
        send("actions.answer", {{"action_id", a_.id}, {"answer", value}});
    }

    // The feed rebuilds the card from the next snapshot once the host has the decision.
    void send(const std::string& method, const json& params) {
        if (busy_) return;
        busy_ = true;
        error_.clear();
        rebuild();
        std::weak_ptr<int> alive = alive_;
        shell_.call(method, params, [this, alive](Result<json> r) {
            if (!alive.lock()) return;
            busy_ = false;
            if (!r) return fail(ui::to_utf16(r.error().message));
            if (a_.kind == "question" && question_writer_) question_writer_->abandon();
            shell_.load_feed();
        });
    }

    void fail(const std::u16string& message) {
        error_ = message;
        shell_.announce(message);
        rebuild();
    }

    Shell& shell_;
    ActionItem a_;
    std::vector<Citation> sources_;
    Mode mode_ = Mode::buttons;
    ui::TextField* field_ = nullptr;
    std::u16string draft_, error_;
    json question_surface_ = nullptr, question_identity_ = nullptr;
    std::shared_ptr<ui::GeneratedSurfaceViewState> question_state_;
    std::shared_ptr<ui::GeneratedSaveQueue> question_writer_;
    bool busy_ = false;
    std::shared_ptr<int> alive_ = std::make_shared<int>(0);
};

// A generated image under its tool card (add-image-generation: Images In The Thread Feed). Until the image is decoded
// a placeholder of its shape holds the place, so the feed does not jump; then the image, scaled to the feed width,
// with Open (full size) and Save as.
class FeedImage : public Node {
public:
    FeedImage(Shell& shell, const std::string& thread, FileRef file, std::string id, int number)
        : Node(std::move(id)), shell_(shell), file_(std::move(file)) {
        role = ui::Role::group;
        gap = 6;
        label_ = shell.t("feed.image.label", {{"width", ui::to_utf16(std::to_string(file_.width))},
                                              {"height", ui::to_utf16(std::to_string(file_.height))}});
        name = label_ + (number > 0 ? u" " + ui::to_utf16(std::to_string(number)) : u"");
        frame_ = emplace<Node>(automation_id() + "-frame");
        auto* row = emplace<Node>();
        row->axis = ui::Axis::row;
        row->gap = 6;
        open_ = row->emplace<ui::Button>(shell.ui(), automation_id() + "-open", shell.t("feed.image.open"), ui::Button::Kind::subtle);
        save_ = row->emplace<ui::Button>(shell.ui(), automation_id() + "-save", shell.t("feed.image.save"), ui::Button::Kind::subtle);
        open_->width = save_->width = Sizing::fit;
        Shell* sp = &shell;
        open_->on_click = [sp, f = file_] { sp->show_image(f); };
        save_->on_click = [sp, f = file_] { sp->save_image(f); };
        show();
        std::weak_ptr<int> alive = alive_;
        shell.load_image(thread, file_, [this, alive] {
            if (!alive.lock()) return;
            show();
        });
    }

private:
    static constexpr float kMaxHeight = 360;

    // The placeholder, the image, or why it could not be loaded.
    void show() {
        frame_->clear_children();
        const Shell::LoadedImage* img = shell_.image(file_.id);
        const bool ready = img && img->bitmap;
        open_->enabled = save_->enabled = ready;
        if (ready) {
            frame_->emplace<ui::ImageView>(label_, img->bitmap, kMaxHeight, automation_id() + "-image");
        } else if (img) {
            frame_->emplace<ui::Label>(shell_.ui(), shell_.t("feed.image.failed"), TextStyle::caption, Paint::error, true,
                                       automation_id() + "-error");
        } else {
            auto* box = frame_->emplace<ui::Box>(automation_id() + "-loading", Paint::surface_alt, 6, Paint::border);
            box->role = ui::Role::image;
            box->name = label_ + u", " + shell_.t("feed.image.loading");
            const float w = static_cast<float>(std::max(1, file_.width)), h = static_cast<float>(std::max(1, file_.height));
            box->measure = [w, h](float avail) {
                float bw = std::min(w, avail), bh = bw * h / w;
                if (bh > kMaxHeight) {
                    bh = kMaxHeight;
                    bw = bh * w / h;
                }
                return ui::Size{bw, bh};
            };
            box->width = Sizing::fit;
        }
        relayout();
        invalidate();
    }

    Shell& shell_;
    FileRef file_;
    std::u16string label_;
    Node* frame_ = nullptr;
    ui::Button* open_ = nullptr;
    ui::Button* save_ = nullptr;
    std::shared_ptr<int> alive_ = std::make_shared<int>(0);
};

}  // namespace

// ---- FeedView --------------------------------------------------------------------------------------------------

FeedView::FeedView(Shell& shell, std::string thread_id) : ScrollView("feed"), shell_(shell), thread_(std::move(thread_id)) {
    role = ui::Role::document;
    name = shell.t("feed.label");
    height = Sizing::fill;
    scrolls = true;
    follow_end = true;  // new output stays in view while the user is at the end
    padding = ui::Insets::xy(20, 16);
    gap = 10;
    std::weak_ptr<int> alive = alive_;
    shell_.call("surfaces.capabilities", json::object(), [this, alive](Result<json> result) {
        if (!alive.lock() || !result || !jbool(*result, "enabled")) return;
        generated_surfaces_enabled_ = true;
        for (auto& slot : slots_) slot.sig.clear();
        update();
    });
}

std::vector<FeedItem> FeedView::visible(std::vector<FeedItem> items) const {
    if (shell_.show_work_details()) return items;
    const auto is_work = [](const FeedItem& it) {
        // The usage line is work detail too (improve-desktop-look D8).
        return it.type == ItemType::reasoning || it.type == ItemType::tool || it.type == ItemType::activity;
    };
    std::set<std::string> active, with_work;
    std::map<std::string, size_t> last;  // run -> index of its last item
    std::map<std::string, std::pair<std::string, std::string>> step;  // run -> step key, MCP server
    for (size_t i = 0; i < items.size(); ++i) {
        const FeedItem& it = items[i];
        if (it.run_id.empty()) continue;
        last[it.run_id] = i;
        if (it.type == ItemType::working || it.streaming) active.insert(it.run_id);
        if (is_work(it)) with_work.insert(it.run_id);
        if (it.type == ItemType::tool) step[it.run_id] = {step_key(it.tool, it.risk), mcp_server(it.tool)};
        else if (it.type == ItemType::reasoning && it.streaming) step[it.run_id] = {"feed.step.thinking", ""};
        else if (it.type == ItemType::assistant) step.erase(it.run_id);  // answering: the generic working line
    }
    std::vector<FeedItem> out;
    out.reserve(items.size());
    for (size_t i = 0; i < items.size(); ++i) {
        FeedItem& it = items[i];
        const std::string run = it.run_id;  // it is moved below
        const bool open = details_.contains(run);
        if (!open && is_work(it)) {
            if (it.type == ItemType::tool && produced_image(it)) {
                it.compact = true;  // the images stay, the card goes
                out.push_back(std::move(it));
            } else if (it.type == ItemType::reasoning && it.streaming) {  // the status line stands in for the trace
                FeedItem w;
                w.type = ItemType::working;
                w.key = run + ":working";
                w.run_id = run;
                w.text = "feed.step.thinking|";
                out.push_back(std::move(w));
            }
        } else {
            // One Tool at work is named in plain words ("Working with mail"); parallel Tools keep their count.
            if (it.type == ItemType::working && (it.text.empty() || it.text == "feed.working_tools|1"))
                if (const auto s = step.find(it.run_id); s != step.end()) it.text = s->second.first + "|" + s->second.second;
            out.push_back(std::move(it));
        }
        if (!run.empty() && last[run] == i && with_work.contains(run) && !active.contains(run)) {
            FeedItem d;
            d.type = ItemType::details;
            d.key = run + ":details";
            d.run_id = run;
            d.text = open ? "open" : "closed";
            out.push_back(std::move(d));
        }
    }
    return out;
}

void FeedView::update() {
    const auto& snap = shell_.feed();
    std::vector<FeedItem> items = visible(snap ? shell_.fold().build(*snap, shell_.live(), shell_.now()) : std::vector<FeedItem>{});
    // A speed line that is waiting for the model or estimating a stream counts on by itself: once a second.
    const bool ticking = std::any_of(items.begin(), items.end(), [](const FeedItem& it) {
        return it.type == ItemType::speed && it.speed && (it.speed->waiting_ms >= 0 || it.speed->approximate);
    });
    if (ticking && !speed_timer_) {
        std::weak_ptr<int> alive = alive_;
        speed_timer_ = shell_.window().set_timer(1000, [this, alive] {
            if (!alive.lock()) return;
            speed_timer_ = 0;  // one-shot
            update();
        });
    }
    // Keyed reconciliation (Live Feed Updates): an element is kept while its key and content signature stay the same,
    // wherever it sits. A change rebuilds only its own item, so other messages keep their text layout, selection and
    // reading position. Streaming text updates in place.
    std::map<std::string, size_t> old_index;
    for (size_t i = 0; i < slots_.size(); ++i) old_index[slots_[i].key] = i;
    std::vector<std::unique_ptr<Node>> old_nodes;
    old_nodes.reserve(children().size());
    while (!children().empty()) old_nodes.push_back(remove(children().back().get()));
    std::reverse(old_nodes.begin(), old_nodes.end());
    std::vector<Slot> next;
    next.reserve(items.size());
    rebuilt_ = 0;
    for (const FeedItem& it : items) {
        const std::string sig = signature(it);
        const auto found = old_index.find(it.key);
        if (found != old_index.end() && found->second < old_nodes.size() && old_nodes[found->second] &&
            slots_[found->second].sig == sig) {
            Slot slot = slots_[found->second];
            if (it.streaming && slot.markdown) slot.markdown->set_markdown(it.text);
            if (it.streaming && slot.text && ui::to_utf8(slot.text->text()) != it.text) slot.text->set_text(ui::to_utf16(it.text));
            add(std::move(old_nodes[found->second]));
            next.push_back(std::move(slot));
            continue;
        }
        Slot slot;
        slot.key = it.key;
        slot.sig = sig;
        add(build(it, slot));
        next.push_back(std::move(slot));
        ++rebuilt_;
        ++rebuilt_total_;
    }
    slots_ = std::move(next);
    relayout();
    invalidate();
    jump();
}

namespace {

// The bubble of a message element: the element itself (the agent's) or its first child (the user's, indented).
ui::Box* bubble(Node* n) {
    if (!n) return nullptr;
    if (auto* b = dynamic_cast<ui::Box*>(n)) return b;
    return n->children().empty() ? nullptr : dynamic_cast<ui::Box*>(n->children().front().get());
}

constexpr UINT kHighlightMs = 2500;

}  // namespace

// Jump to message (Full-Text Search): once the route's message is in the feed, scroll it to the middle of the feed
// and highlight it for a moment.
void FeedView::jump() {
    const Route& r = shell_.route();
    if (!r.msg || r.thread_id != thread_ || (jumped_ && *jumped_ == *r.msg)) return;
    const std::string key = r.msg->run_id + ":" + std::to_string(r.msg->seq);
    auto find = [this](const std::string& k) -> Node* {
        for (size_t i = 0; i < slots_.size() && i < children().size(); ++i)
            if (slots_[i].key == k) return children()[i].get();
        return nullptr;
    };
    ui::Box* box = bubble(find(key));
    if (!box) return;  // not in this snapshot (yet)
    jumped_ = *r.msg;
    if (!highlighted_.empty())
        if (ui::Box* old = bubble(find(highlighted_))) old->border = Paint::border;
    highlighted_ = key;
    box->border = Paint::accent;
    box->invalidate();
    std::weak_ptr<int> alive = alive_;
    // After the layout pass that places the new items (timer messages come after posted ones).
    if (jump_timer_) shell_.window().cancel_timer(jump_timer_);
    jump_timer_ = shell_.window().set_timer(1, [this, alive, key, find] {
        if (!alive.lock()) return;
        jump_timer_ = 0;
        Node* n = find(key);
        ui::Tree* t = Node::tree();
        if (!n || !t) return;
        const float y = scroll_y() + (n->frame().y - frame().y) - std::max(0.f, (frame().h - n->frame().h) / 2);
        t->scroll_to(*this, y);
        invalidate();
    });
    if (highlight_timer_) shell_.window().cancel_timer(highlight_timer_);
    highlight_timer_ = shell_.window().set_timer(kHighlightMs, [this, alive, key, find] {
        if (!alive.lock()) return;
        highlight_timer_ = 0;
        if (highlighted_ != key) return;
        highlighted_.clear();
        if (ui::Box* b = bubble(find(key))) {
            b->border = Paint::border;
            b->invalidate();
        }
    });
}

// The sources of one answer or tool result: a numbered list of links that open each passage (Cited Sources In The Feed).
void FeedView::add_citations(Node& wrap, const std::vector<Citation>& citations, const std::string& key, const char* list_prefix,
                             const char* item_prefix) {
    ui::Ui& u = shell_.ui();
    auto* list = wrap.emplace<Node>(id_of(list_prefix, key));
    list->role = ui::Role::list;
    list->name = shell_.t("feed.citations");
    list->gap = 2;
    list->padding.left = 16;
    for (size_t i = 0; i < citations.size(); ++i) {
        const Citation& c = citations[i];
        const std::string source_key = "settings.knowledge.source." + c.source;
        std::u16string label = u"[" + ui::to_utf16(std::to_string(c.n ? c.n : static_cast<long long>(i) + 1)) + u"] " +
                               ui::to_utf16(c.title.empty() ? c.ref : c.title);
        if (shell_.ui().strings.has(source_key)) label += u" · " + shell_.t(source_key);
        if (c.date.size() >= 10) label += u" · " + ui::to_utf16(c.date.substr(0, 10));
        auto* link = list->emplace<ui::Button>(u, id_of(item_prefix, key) + "-" + std::to_string(i), label, ui::Button::Kind::subtle);
        link->width = Sizing::fit;
        link->role = ui::Role::link;
        link->name = shell_.t("feed.open_citation", {{"name", label}});
        const std::string ref = c.ref;
        ui::Ui* up = &u;
        link->on_click = [this, up, ref] {
            if (ref.starts_with("skowork://thread/")) {
                shell_.navigate(thread_hash(ref.substr(sizeof("skowork://thread/") - 1)));
            } else if (ref.starts_with("https://") || ref.starts_with("http://")) {
                if (up->open_link) up->open_link(ui::to_utf16(ref));  // asks first, shows the URL
            } else {
                // A file of the files Source: the host opens it only if it is an indexed item.
                shell_.call("knowledge.open", {{"ref", ref}}, [this](Result<json> r) {
                    if (!r) shell_.announce(shell_.t("feed.citation_unavailable", {{"reason", ui::to_utf16(r.error().message)}}));
                });
            }
        };
    }
}

// Images a tool call stored, and with `citations` the passages knowledge.search returned, under its card (or alone with
// details hidden, where only the images stay).
void FeedView::add_outputs(Node& out, const FeedItem& it, bool citations) {
    Node* wrap = &out;
    std::vector<const FileRef*> images;
    if (it.result)
        for (const FileRef& f : it.result->files)
            if (f.image()) images.push_back(&f);
    if (!images.empty()) {
        auto* list = wrap->emplace<Node>(id_of("tool-images-", it.key));
        list->gap = 10;
        list->padding.left = 16;
        for (size_t i = 0; i < images.size(); ++i)
            list->emplace<FeedImage>(shell_, thread_, *images[i], id_of("tool-image-", it.key) + "-" + std::to_string(i),
                                     images.size() > 1 ? static_cast<int>(i) + 1 : 0);
    }
    if (citations && it.result && !it.result->citations.empty())
        add_citations(*wrap, it.result->citations, it.key, "tool-citations-", "tool-citation-");
    // A saved Skill is named in the Thread with a link to its Publish dialog; the user shares there (design D11).
    if (it.result && !it.result->publish_route.empty()) {
        auto* link = wrap->emplace<ui::Button>(shell_.ui(), id_of("tool-publish-", it.key), shell_.t("feed.skill_publish"), ui::Button::Kind::subtle);
        link->width = Sizing::fit;
        link->role = ui::Role::link;
        const std::string route = it.result->publish_route;
        link->on_click = [this, route] { shell_.navigate(route); };
    }
}

std::unique_ptr<Node> FeedView::build(const FeedItem& it, Slot& slot) {
    ui::Ui& u = shell_.ui();
    switch (it.type) {
    case ItemType::user: {
        auto row = std::make_unique<Node>(id_of("msg-", it.key));
        row->padding.left = 48;
        auto* msg = row->emplace<ui::Box>(std::string(), Paint::accent_surface, 8, Paint::border);  // the user's own, tinted
        msg->padding = ui::Insets::xy(14, 8);
        msg->gap = 4;
        msg->role = ui::Role::group;
        msg->name = shell_.t("feed.you");
        message_header(shell_, *msg, it);
        msg->emplace<ui::MarkdownView>(u, shell_.t("feed.you"), id_of("msg-text-", it.key))->set_plain(ui::to_utf16(it.text));
        return row;
    }
    case ItemType::assistant: {
        auto msg = std::make_unique<ui::Box>(id_of("msg-", it.key), Paint::surface, 8, Paint::border);
        msg->padding = ui::Insets::xy(14, 8);
        msg->gap = 4;
        msg->role = ui::Role::group;
        msg->name = shell_.t("feed.agent");
        message_header(shell_, *msg, it);
        const auto blocks = generated_surfaces_enabled_ ? eligible_generated_surfaces(it) : std::vector<ui::GeneratedSurfaceBlock>{};
        bool supported = !blocks.empty();
        for (const auto& block : blocks)
            if (!ui::generated_surface_presentation_supported(block.surface, true, true)) supported = false;
        if (supported) {
            bool ready = true;
            for (size_t i = 0; i < blocks.size(); ++i) {
                const std::string key = surface_cache_key(it, i, blocks[i]);
                auto [entry, inserted] = surfaces_.try_emplace(key);
                if (inserted) {
                    const json identity = {{"thread_id", thread_}, {"run_id", it.run_id}, {"event_seq", it.seq}, {"block_index", i}};
                    const json surface = blocks[i].surface;
                    const std::string item_key = it.key;
                    std::weak_ptr<int> alive = alive_;
                    shell_.call("surfaces.open", identity, [this, alive, key, item_key, surface, identity](Result<json> result) {
                        if (!alive.lock()) return;
                        auto found = surfaces_.find(key);
                        if (found == surfaces_.end()) return;
                        if (!result || jstr(*result, "revision").empty()) found->second.failed = true;
                        else {
                            found->second.revision = jstr(*result, "revision");
                            found->second.accepted = jbool(*result, "accepted");
                            auto bound = identity;
                            bound["revision"] = found->second.revision;
                            auto writer = shell_.generated_writer("surfaces.save", bound);
                            const json saved = writer->latest().value_or(result->value("state", json(nullptr)));
                            found->second.state = std::make_shared<ui::GeneratedSurfaceViewState>(
                                ui::generated_surface_view_state_from_json(surface, saved));
                            found->second.ready = true;
                        }
                        for (auto& cached : slots_) if (cached.key == item_key) cached.sig.clear();
                        update();
                    });
                }
                if (!entry->second.ready || entry->second.failed) ready = false;
            }
            if (ready) {
                size_t offset = 0;
                for (size_t i = 0; i < blocks.size(); ++i) {
                    const auto& block = blocks[i];
                    if (block.begin > offset) {
                        auto* markdown = msg->emplace<ui::MarkdownView>(u, shell_.t("feed.agent"));
                        markdown->set_markdown(it.text.substr(offset, block.begin - offset));
                    }
                    const std::string key = surface_cache_key(it, i, block);
                    const auto state = surfaces_.at(key).state;
                    json identity = {{"thread_id", thread_}, {"run_id", it.run_id}, {"event_seq", it.seq},
                                     {"block_index", i}, {"revision", surfaces_.at(key).revision}};
                    const auto writer = shell_.generated_writer("surfaces.save", identity);
                    std::weak_ptr<int> alive = alive_;
                    auto changed = [alive, state, writer] {
                        if (!alive.lock()) return;
                        writer->save(ui::generated_surface_view_state_to_json(*state));
                    };
                    ui::GeneratedActionView::Submit submit = [this, alive, state, identity, key, item_key = it.key](std::string button,
                            std::vector<ui::GeneratedFieldValue> fields, std::function<void(bool)> done) {
                        if (!alive.lock()) return done(false);
                        auto params = identity;
                        params["button_id"] = std::move(button);
                        params["draft"] = json::object();
                        for (const auto& field : fields) params["draft"][field.id] = field.value;
                        params["sections"] = ui::generated_surface_view_state_to_json(*state)["sections"];
                        auto send = [this, alive, params, done, key, item_key](const std::string& mode) mutable {
                            if (!alive.lock()) return done(false);
                            if (!mode.empty()) params["mode"] = mode;
                            shell_.call("surfaces.submit", params, [this, alive, done, key, item_key](Result<json> result) {
                                done(result.ok());
                                if (!alive.lock() || !result) return;
                                const auto found = surfaces_.find(key);
                                if (found == surfaces_.end()) return;
                                found->second.accepted = true;
                                for (auto& cached : slots_) if (cached.key == item_key) cached.sig.clear();
                                update();
                            });
                        };
                        const auto& snapshot = shell_.feed();
                        if (!snapshot || snapshot->entry.id != thread_ || !active_run(*snapshot)) return send("");
                        auto dialog = std::make_unique<ui::Dialog>(shell_.ui(), "generated-delivery", shell_.t("generated.action.delivery"));
                        dialog->body().emplace<ui::Label>(shell_.ui(), shell_.t("generated.action.delivery_body"), ui::TextStyle::body, ui::Paint::text, true);
                        auto* dp = dialog.get();
                        dialog->add_button("generated-delivery-queue", shell_.t("composer.queue"), ui::Button::Kind::primary, [dp, send] {
                            auto deliver = send;
                            dp->close();
                            deliver("queue");
                        });
                        dialog->add_button("generated-delivery-steer", shell_.t("composer.steer"), ui::Button::Kind::standard, [dp, send] {
                            auto deliver = send;
                            dp->close();
                            deliver("steer_now");
                        });
                        auto cancel = [dp, done] { auto finish = done; dp->close(); finish(false); };
                        dialog->add_button("generated-delivery-cancel", shell_.t("thread.cancel"), ui::Button::Kind::standard, cancel);
                        dialog->on_cancel = cancel;
                        shell_.window().modal().show(std::move(dialog));
                        shell_.window().render();
                    };
                    auto resolver = surface_citation_resolver(shell_, alive, it.surface_sources);
                    msg->emplace<ui::GeneratedSurfaceView>(u, block.surface, state, changed, submit, resolver,
                                                           surfaces_.at(key).accepted);
                    offset = block.end;
                }
                if (offset < it.text.size()) {
                    auto* markdown = msg->emplace<ui::MarkdownView>(u, shell_.t("feed.agent"));
                    markdown->set_markdown(it.text.substr(offset));
                }
                if (!it.sources.empty()) add_citations(*msg, it.sources, it.key, "msg-sources-", "msg-source-");
                return msg;
            }
        }
        slot.markdown = msg->emplace<ui::MarkdownView>(u, shell_.t("feed.agent"), id_of("md-", it.key));
        slot.markdown->set_markdown(it.text);
        if (!it.sources.empty()) add_citations(*msg, it.sources, it.key, "msg-sources-", "msg-source-");
        return msg;
    }
    case ItemType::reasoning: {
        const std::string key = it.key;
        auto card = std::make_unique<ui::Card>(u, id_of("reasoning-", it.key), shell_.t(it.streaming ? "feed.thinking" : "feed.reasoning"),
                                               true, expanded_.contains(key));
        card->background = Paint::none;
        card->on_expand = [this, key](bool on) {
            if (on) expanded_.insert(key);
            else expanded_.erase(key);
        };
        slot.text = card->body().emplace<ui::Label>(u, ui::to_utf16(it.text), TextStyle::body, Paint::text_secondary, true);
        return card;
    }
    case ItemType::tool: {
        const std::string key = it.key;
        if (it.compact) {  // work details hidden: only the images the call stored
            auto wrap = std::make_unique<Node>(id_of("tool-item-", it.key));
            wrap->gap = 8;
            add_outputs(*wrap, it, false);
            return wrap;
        }
        auto card = std::make_unique<ui::Card>(u, id_of("tool-", it.key), shell_.t("feed.tool.label", {{"tool", ui::to_utf16(it.tool)}}),
                                               true, expanded_.contains(key));
        card->on_expand = [this, key](bool on) {
            if (on) expanded_.insert(key);
            else expanded_.erase(key);
        };
        // An unknown outcome is neither a result nor an error (Recovery Choice Presentation): it says only that it is unknown.
        const bool unknown = it.result && it.result->unknown;
        const bool bad = it.result ? !it.result->ok && !unknown : it.decision == "deny";
        const char* state_key = !it.result ? "feed.tool.pending" : unknown ? "feed.tool.unknown" : it.result->ok ? "feed.tool.result" : "feed.tool.error";
        const std::u16string state = shell_.t(state_key);
        card->header().emplace<ui::Badge>(u, state, bad ? Paint::error_background : unknown ? Paint::warning_background : it.result ? Paint::surface_alt : Paint::info_background,
                                          Paint::text, id_of("tool-state-", it.key));
        if (it.edited)
            card->header().emplace<ui::Badge>(u, shell_.t("feed.tool.edited"), Paint::warning_background, Paint::text,
                                              id_of("tool-edited-", it.key));
        // The header's name is what a screen reader reads for the collapsed card.
        card->header().name = shell_.t("feed.tool.label", {{"tool", ui::to_utf16(it.tool)}}) + u", " + state +
                              (it.edited ? u", " + shell_.t("feed.tool.edited") : u"");
        Node& b = card->body();
        const std::u16string risk = shell_.ui().strings.has("feed.risk." + it.risk) ? shell_.t("feed.risk." + it.risk) : ui::to_utf16(it.risk);
        b.emplace<ui::Label>(u, shell_.t("feed.tool.risk") + u": " + risk, TextStyle::body, Paint::text, true, id_of("tool-risk-", it.key));
        if (!it.decision.empty() && it.decision != "allow") {
            const std::string dk = "feed.tool.decision." + it.decision;
            std::u16string d = shell_.ui().strings.has(dk) ? shell_.t(dk) : ui::to_utf16(it.decision);
            if (!it.reason.empty()) d += u": " + ui::to_utf16(it.reason);
            b.emplace<ui::Label>(u, d, TextStyle::body, Paint::text, true, id_of("tool-decision-", it.key));
        }
        b.emplace<ui::Label>(u, shell_.t("feed.tool.args"), TextStyle::caption, Paint::text_secondary, true);
        code_box(shell_, b, id_of("tool-args-", it.key), it.args.dump(2));
        if (it.result) {
            b.emplace<ui::Label>(u, shell_.t(state_key), TextStyle::caption, Paint::text_secondary, true);
            code_box(shell_, b, id_of("tool-result-", it.key), it.result->content, it.result->ok || unknown ? Paint::text : Paint::error);
        }
        // Images the tool stored, and the sources knowledge.search cited, show under the card without expanding it.
        if (!produced_something(it)) return card;
        auto wrap = std::make_unique<Node>(id_of("tool-item-", it.key));
        wrap->gap = 8;
        wrap->add(std::move(card));
        add_outputs(*wrap, it, true);
        return wrap;
    }
    case ItemType::action:
        return std::make_unique<ActionCard>(shell_, it.action, generated_surfaces_enabled_, it.surface_sources);
    case ItemType::activity:
        if (it.usage) {
            const auto n = [](long long v) { return ui::to_utf16(std::to_string(v)); };
            std::u16string text = shell_.t("feed.run_usage", {{"prompt", n(it.usage->prompt)},
                                                              {"completion", n(it.usage->completion)},
                                                              {"calls", n(it.usage->calls)}});
            if (it.usage->reasoning > 0) text += u" " + shell_.t("feed.run_usage_reasoning", {{"reasoning", n(it.usage->reasoning)}});
            return std::make_unique<ui::Label>(u, text, TextStyle::caption, Paint::text_secondary, true, id_of("usage-", it.run_id));
        }
        return std::make_unique<ui::Label>(u, ui::to_utf16(it.text), TextStyle::caption, Paint::text_secondary, true, id_of("activity-", it.key));
    case ItemType::error:
        return std::make_unique<ui::Label>(u, shell_.t("feed.error_line", {{"code", ui::to_utf16(it.error.code.empty() ? "?" : it.error.code)}}),
                                           TextStyle::caption, Paint::error, true, id_of("error-", it.key));
    case ItemType::stopped:
        return std::make_unique<ui::Label>(u, it.by_user ? shell_.t("feed.stopped_by_user") : shell_.t("feed.cancelled", {{"reason", ui::to_utf16(it.reason)}}),
                                           TextStyle::caption, Paint::text_secondary, true, id_of("stopped-", it.key));
    case ItemType::limit: {  // Run Limit Note: shown whatever Work Details Visibility hides
        const char* key = it.reason == "tool call limit reached"    ? "feed.limit_tool_calls"
                          : it.reason == "model call limit reached" ? "feed.limit_model_calls"
                          : it.reason == "wall time limit reached"  ? "feed.limit_minutes"
                                                                    : "feed.limit_other";
        return std::make_unique<ui::Label>(u, shell_.t(key, {{"count", ui::to_utf16(it.text)}}), TextStyle::caption, Paint::text_secondary, true,
                                           id_of("limit-", it.key));
    }
    case ItemType::working: {
        const auto bar = it.text.find('|');
        const std::string k = it.text.empty() ? "feed.working" : it.text.substr(0, bar);
        const std::u16string server = bar == std::string::npos ? u"" : ui::to_utf16(it.text.substr(bar + 1));
        auto l = std::make_unique<ui::Label>(u, shell_.t(k, {{"server", server}, {"count", server}}), TextStyle::caption, Paint::text_secondary, true,
                                             id_of("working-", it.key));
        return l;
    }
    case ItemType::speed: {  // Generation Speed Under Answers: plain text, not a live region (design D4)
        const RunSpeed sp = it.speed.value_or(RunSpeed{});
        const std::u16string rate = ui::to_utf16(std::to_string(std::max(0LL, sp.tokens_per_s)));
        std::u16string text, spoken;
        if (sp.waiting_ms >= 0) {
            text = shell_.t("feed.speed_waiting", {{"seconds", ui::to_utf16(std::to_string(sp.waiting_ms / 1000))}});
            spoken = text;
            if (sp.tokens_per_s >= 0) {
                const std::u16string done = shell_.t(sp.approximate ? "feed.speed_approx" : "feed.speed", {{"rate", rate}});
                text = done + u" · " + text;
                spoken = shell_.t(sp.approximate ? "feed.speed_approx_name" : "feed.speed_name", {{"rate", rate}}) + u". " + spoken;
            }
        } else {
            text = shell_.t(sp.approximate ? "feed.speed_approx" : "feed.speed", {{"rate", rate}});
            spoken = shell_.t(sp.approximate ? "feed.speed_approx_name" : "feed.speed_name", {{"rate", rate}});
        }
        auto l = std::make_unique<ui::Label>(u, text, TextStyle::caption, Paint::text_secondary, true, id_of("speed-", it.run_id));
        l->name = spoken;
        return l;
    }
    case ItemType::details: {
        const bool open = it.text == "open";
        auto b = std::make_unique<ui::Button>(u, id_of("details-", it.run_id), shell_.t(open ? "feed.hide_details" : "feed.show_details"),
                                              ui::Button::Kind::subtle);
        b->width = Sizing::fit;
        b->name = shell_.t(open ? "feed.hide_details_name" : "feed.show_details_name");
        std::weak_ptr<int> alive = alive_;
        b->on_click = [this, alive, run = it.run_id] {
            // After the click returns: the update replaces this button.
            shell_.window().set_timer(1, [this, alive, run] {
                if (!alive.lock()) return;
                if (!details_.erase(run)) details_.insert(run);
                update();
            });
        };
        return b;
    }
    }
    return std::make_unique<Node>();
}

}  // namespace wb::shell
