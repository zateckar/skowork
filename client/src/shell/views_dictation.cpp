// Dictation in the composers (client/dictation; add-dictation design D3). The host records and transcribes
// (dictation.start, .stop, .cancel); the window keeps one recording, its owner and the elapsed time.
#include "shell/shell.h"
#include "ui/text16.h"

#include <algorithm>

namespace wb::shell {

using ui::Paint;
using ui::Sizing;
using ui::TextStyle;

namespace {

std::u16string elapsed(long long ms) {
    const long long s = std::max(0LL, ms / 1000);
    const long long sec = s % 60;
    return ui::to_utf16(std::to_string(s / 60) + (sec < 10 ? ":0" : ":") + std::to_string(sec));
}

bool space(char16_t c) { return c == u' ' || c == u'\t' || c == u'\r' || c == u'\n'; }

}  // namespace

// ---- the window's recording ------------------------------------------------------------------------------------

void Shell::start_dictation(const std::string& owner, std::weak_ptr<int> alive, std::function<ui::TextField*()> field) {
    if (dictation_.phase != DictationPhase::idle) return;
    dictation_ = {owner, DictationPhase::starting, 0, std::move(alive), std::move(field), 0};
    dictation_changed();
    call("dictation.start", json::object(), [this, owner](Result<json> r) {
        if (dictation_.phase != DictationPhase::starting || dictation_.owner != owner) {
            if (r) call("dictation.cancel", json::object());  // cancelled while it was starting
            return;
        }
        if (!r) {
            dictation_ = {};
            announce(t("composer.dictation_failed", {{"why", ui::to_utf16(r.error().message)}}));
            dictation_changed();
            return;
        }
        dictation_.phase = DictationPhase::recording;
        dictation_.started = now();
        announce(t("composer.dictation_started"));
        // The composer keeps the keyboard focus: Escape there discards the recording.
        if (ui::TextField* f = dictation_.alive.expired() || !dictation_.field ? nullptr : dictation_.field())
            window().tree().set_focus(f);
        dictation_changed();
        dictation_.timer = window().set_timer(1000, [this] { dictation_tick(); });
    });
}

void Shell::dictation_tick() {
    dictation_.timer = 0;
    if (dictation_.phase != DictationPhase::recording) return;
    if (dictation_.alive.expired()) {  // the composer is gone: nothing would take the text
        cancel_dictation();
        return;
    }
    dictation_changed();
    dictation_.timer = window().set_timer(1000, [this] { dictation_tick(); });
}

void Shell::stop_dictation() {
    if (dictation_.phase != DictationPhase::recording) return;
    if (dictation_.timer) window().cancel_timer(dictation_.timer);
    dictation_.timer = 0;
    dictation_.phase = DictationPhase::transcribing;
    announce(t("composer.transcribing"));
    dictation_changed();
    call("dictation.stop", json::object(), [this](Result<json> r) {
        DictationRun run = std::move(dictation_);
        dictation_ = {};
        dictation_changed();
        if (!r) {
            announce(t("composer.dictation_failed", {{"why", ui::to_utf16(r.error().message)}}));
            return;
        }
        const std::u16string text = ui::to_utf16(jstr(*r, "text"));
        if (text.empty()) {
            announce(t("composer.dictation_empty"));
            return;
        }
        ui::TextField* f = run.alive.expired() || !run.field ? nullptr : run.field();
        if (!f) {
            announce(t("composer.dictation_discarded"));
            return;
        }
        // At the caret, separated from the text before it; nothing is sent.
        const char16_t before = f->char_before_caret();
        f->insert(before == 0 || space(before) ? text : u" " + text);
        window().tree().set_focus(f);
        announce(t("composer.dictation_inserted"));
    });
}

void Shell::cancel_dictation() {
    const DictationPhase phase = dictation_.phase;
    if (phase != DictationPhase::recording && phase != DictationPhase::starting) return;
    if (dictation_.timer) window().cancel_timer(dictation_.timer);
    dictation_ = {};
    if (phase == DictationPhase::recording) call("dictation.cancel", json::object());
    announce(t("composer.dictation_discarded"));
    dictation_changed();
}

void Shell::discard_dictation() {
    if (dictation_.phase != DictationPhase::recording && dictation_.phase != DictationPhase::starting) return;
    if (dictation_.timer) window().cancel_timer(dictation_.timer);
    dictation_ = {};
    announce(t("composer.dictation_discarded"));
    dictation_changed();
}

bool Shell::cancel_dictation_of(const std::string& owner) {
    if (dictation_.owner != owner || dictation_.phase != DictationPhase::recording) return false;
    cancel_dictation();
    return true;
}

void Shell::watch_dictation(DictateControl* c) { dictate_controls_.push_back(c); }

void Shell::unwatch_dictation(DictateControl* c) { std::erase(dictate_controls_, c); }

void Shell::dictation_changed() {
    const auto controls = dictate_controls_;  // an update may rebuild a composer
    for (DictateControl* c : controls)
        if (std::find(dictate_controls_.begin(), dictate_controls_.end(), c) != dictate_controls_.end()) c->update();
}

// ---- the Dictate control ---------------------------------------------------------------------------------------

DictateControl::DictateControl(Shell& shell, std::string id, std::string owner, std::weak_ptr<int> alive,
                               std::function<ui::TextField*()> field)
    : Node(id + "-row"), shell_(shell), id_(std::move(id)), owner_(std::move(owner)), alive_(std::move(alive)), field_(std::move(field)) {
    axis = ui::Axis::row;
    gap = 4;
    width = Sizing::fit;
    shell_.watch_dictation(this);
    update();
}

DictateControl::~DictateControl() { shell_.unwatch_dictation(this); }

void DictateControl::update() {
    const json d = shell_.status().value("dictation", json::object());
    const bool on = jbool(d, "enabled");
    const bool available = jbool(d, "available");
    const std::string reason = jstr(d, "reason");
    const bool mine = shell_.dictation_owner() == owner_;
    const Shell::DictationPhase phase = mine ? shell_.dictation_phase() : Shell::DictationPhase::idle;
    const bool other = !mine && shell_.dictation_phase() != Shell::DictationPhase::idle;  // another composer records
    const std::string shown = std::to_string(static_cast<int>(phase)) + (on ? "e" : "") + (available ? "a" : "") +
                              (other ? "o" : "") + reason + shell_.ui().strings.lang();
    if (phase == Shell::DictationPhase::recording && shown == shown_ && time_) {
        time_->set_text(shell_.t("composer.dictating", {{"time", elapsed(shell_.now() - shell_.dictation_started())}}));
        time_->invalidate();
        return;
    }
    if (shown == shown_) return;
    shown_ = shown;
    ui::Tree* tr = Node::tree();
    bool had_focus = false;  // a button here keeps the focus when the control changes
    for (Node* n = tr ? tr->focused() : nullptr; n; n = n->parent())
        if (n == this) had_focus = true;
    clear_children();
    time_ = nullptr;
    visible = on;
    ui::Ui& u = shell_.ui();
    const std::string& id = id_;
    ui::Node* focus = nullptr;
    if (phase == Shell::DictationPhase::recording) {
        auto* stop = emplace<ui::IconButton>(u, id + "-stop", ui::icon::stop, shell_.t("composer.dictate_stop"), ui::Button::Kind::primary);
        stop->on_click = [this] { shell_.stop_dictation(); };
        time_ = emplace<ui::Label>(u, shell_.t("composer.dictating", {{"time", elapsed(shell_.now() - shell_.dictation_started())}}),
                                   TextStyle::caption, Paint::text_secondary, false, id + "-time");
        time_->padding.top = 7;
        auto* cancel = emplace<ui::IconButton>(u, id + "-cancel", ui::icon::cancel, shell_.t("composer.dictate_cancel"));
        cancel->on_click = [this] { shell_.cancel_dictation(); };
        focus = stop;
    } else if (phase == Shell::DictationPhase::transcribing) {
        auto* l = emplace<ui::Label>(u, shell_.t("composer.transcribing"), TextStyle::caption, Paint::text_secondary, false, id + "-progress");
        l->padding.top = 7;
    } else {
        const std::u16string label = available || reason.empty() ? shell_.t("composer.dictate") : shell_.t("composer.dictation_off." + reason);
        auto* b = emplace<ui::IconButton>(u, id, ui::icon::microphone, label);
        b->enabled = available && !other && phase == Shell::DictationPhase::idle;
        std::weak_ptr<int> alive = alive_;
        b->on_click = [this, alive] { shell_.start_dictation(owner_, alive, field_); };
        focus = b->enabled ? b : nullptr;
    }
    if (tr && had_focus && focus) tr->set_focus(focus);
    relayout();
    invalidate();
}

}  // namespace wb::shell
