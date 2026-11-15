#include "ui/generated_action_view.h"

#include "ui/text16.h"

namespace wb::ui {
namespace {
json snapshot(const std::vector<GeneratedFieldValue>& fields) {
    json result = json::object();
    for (const auto& field : fields) result[field.id] = field.value;
    return result;
}

std::u16string display_value(Ui& ui, const json& value) {
    if (value.is_null()) return ui.strings.get("generated.action.not_selected");
    if (value.is_boolean()) return ui.strings.get(value.get<bool>() ? "generated.action.on" : "generated.action.off");
    if (value.is_string()) return to_utf16(value.get<std::string>());
    if (value.is_array()) {
        std::u16string result;
        for (const auto& item : value) {
            if (!result.empty()) result += u", ";
            result += to_utf16(item.get<std::string>());
        }
        return result.empty() ? ui.strings.get("generated.action.not_selected") : result;
    }
    return to_utf16(value.dump());
}
}  // namespace

GeneratedActionView::GeneratedActionView(Ui& ui, json action, json surface, std::shared_ptr<json> draft, Submit submit,
                                         std::function<json()> sections)
    : Box(jstr(action, "id")), ui_(ui), action_(std::move(action)), surface_(std::move(surface)),
      draft_(draft ? std::move(draft) : std::make_shared<json>(json::object())), submit_(std::move(submit)), sections_(std::move(sections)) {
    gap = 6;
    review_button_ = emplace<Button>(ui_, automation_id() + "-review", to_utf16(jstr(action_, "label")), Button::Kind::primary);
    review_button_->enabled = static_cast<bool>(submit_);
    review_button_->on_click = [this] { review(); };
}

void GeneratedActionView::close_review() {
    if (!panel_) return;
    auto removed = remove(panel_);
    panel_ = nullptr;
    status_ = nullptr;
    confirm_ = nullptr;
    cancel_ = nullptr;
}

void GeneratedActionView::show_status(const char* key, bool success) {
    const auto message = ui_.strings.get(key);
    if (status_) {
        status_->color = success ? Paint::text : Paint::error;
        status_->set_text(message);
        status_->visible = true;
        status_->relayout();
    }
    if (ui_.announce) ui_.announce(message);
}

void GeneratedActionView::review() {
    if (accepted_ || pending_) return;
    if (!generated_surface_sections_allow_action(surface_, sections_ ? sections_() : json::object(), jstr(action_, "id"))) {
        close_review();
        auto* panel = emplace<Box>(automation_id() + "-review-panel", Paint::surface_alt, 6, Paint::border);
        panel_ = panel;
        panel->padding = Insets::all(8);
        status_ = panel->emplace<Label>(ui_, u"", TextStyle::body, Paint::error, true, automation_id() + "-status");
        show_status("generated.action.expand");
        return;
    }
    const auto fields = generated_surface_values(surface_, *draft_);
    if (!fields) {
        close_review();
        auto* panel = emplace<Box>(automation_id() + "-review-panel", Paint::surface_alt, 6, Paint::border);
        panel_ = panel;
        panel->padding = Insets::all(8);
        status_ = panel->emplace<Label>(ui_, u"", TextStyle::body, Paint::error, true, automation_id() + "-status");
        show_status("generated.action.invalid");
        return;
    }
    close_review();
    reviewed_ = snapshot(*fields);
    auto* panel = emplace<Box>(automation_id() + "-review-panel", Paint::surface_alt, 6, Paint::border);
    panel_ = panel;
    panel->padding = Insets::all(8);
    panel->gap = 6;
    panel->role = Role::group;
    panel->name = ui_.strings.get("generated.action.review", {{"action", to_utf16(jstr(action_, "label"))}});
    panel->emplace<Label>(ui_, panel->name, TextStyle::body_strong);
    for (const auto& field : *fields)
        panel->emplace<Label>(ui_, to_utf16(field.label) + u": " + display_value(ui_, field.value), TextStyle::body);
    status_ = panel->emplace<Label>(ui_, u"", TextStyle::body, Paint::error, true, automation_id() + "-status");
    status_->visible = false;
    confirm_ = panel->emplace<Button>(ui_, automation_id() + "-confirm", ui_.strings.get("generated.action.confirm"), Button::Kind::primary);
    confirm_->on_click = [this] {
        if (accepted_ || pending_) return;
        const auto current = generated_surface_values(surface_, *draft_);
        if (!current || snapshot(*current) != reviewed_ ||
            !generated_surface_sections_allow_action(surface_, sections_ ? sections_() : json::object(), jstr(action_, "id"))) {
            show_status("generated.action.changed");
            return;
        }
        const auto submit = submit_;
        const auto id = jstr(action_, "id");
        const auto alive = std::weak_ptr<int>(alive_);
        const uint64_t request = ++request_;
        pending_ = true;
        confirm_->enabled = false;
        cancel_->enabled = false;
        review_button_->enabled = false;
        show_status("generated.action.pending", true);
        if (alive.expired()) return;
        submit(id, *current, [this, alive, request](bool accepted) {
            if (alive.expired() || !pending_ || request != request_) return;
            pending_ = false;
            accepted_ = accepted;
            confirm_->enabled = !accepted;
            cancel_->enabled = !accepted;
            review_button_->enabled = !accepted;
            show_status(accepted ? "generated.action.accepted" : "generated.action.failed", accepted);
        });
    };
    cancel_ = panel->emplace<Button>(ui_, automation_id() + "-cancel", ui_.strings.get("generated.action.cancel"));
    cancel_->on_click = [this] { if (!pending_) close_review(); };
}

}  // namespace wb::ui
