#include "ui/generated_slider.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "base/json.h"
#include "ui/text16.h"

namespace wb::ui {

GeneratedSlider::GeneratedSlider(Ui& ui, std::string id, std::u16string label, double minimum, double maximum, double value)
    : Node(std::move(id)), ui_(ui), minimum_(minimum), maximum_(maximum), value_(value) {
    role = Role::slider;
    name = std::move(label);
    focusable = true;
    height = Sizing::fixed;
    fixed_h = 58;
}

double GeneratedSlider::range_step() const {
    return std::max(std::numeric_limits<double>::denorm_min(), maximum_ / 100 - minimum_ / 100);
}

bool GeneratedSlider::set_range_value(double value) {
    if (!enabled || !std::isfinite(value) || value < minimum_ || value > maximum_) return false;
    if (value == value_) return true;
    value_ = value;
    invalidate();
    auto notify_change = on_change;
    if (notify_change) notify_change(value);
    return true;
}

double GeneratedSlider::fraction() const {
    const long double lo = minimum_, hi = maximum_, current = value_;
    return std::clamp(static_cast<double>((current - lo) / (hi - lo)), 0.0, 1.0);
}

void GeneratedSlider::set_fraction(double position) {
    (void)set_range_value(std::lerp(minimum_, maximum_, std::clamp(position, 0.0, 1.0)));
}

bool GeneratedSlider::on_key(const KeyEvent& event) {
    if (!enabled) return false;
    if (event.key == kKeyHome) set_fraction(0);
    else if (event.key == kKeyEnd) set_fraction(1);
    else if (event.key == kKeyRight || event.key == kKeyUp) set_fraction(fraction() + 0.01);
    else if (event.key == kKeyLeft || event.key == kKeyDown) set_fraction(fraction() - 0.01);
    else return false;
    return true;
}

bool GeneratedSlider::on_pointer(const PointerEvent& event) {
    if (!enabled) return false;
    if (event.kind == PointerEvent::down && event.button != 0) return false;
    if (event.kind == PointerEvent::down || (event.kind == PointerEvent::move && pressed_)) {
        const float available = std::max(1.f, frame().w - 16);
        set_fraction(static_cast<double>((event.x - frame().x - 8) / available));
        return true;
    }
    return event.kind == PointerEvent::up && pressed_ && event.button == 0;
}

void GeneratedSlider::draw(Canvas& canvas) {
    const auto title = name + u": " + to_utf16(json(value_).dump());
    canvas.text(ui_.layout(title, TextStyle::body, frame().w, false), frame().x, frame().y, Paint::text);
    const float left = frame().x + 8, right = frame().right() - 8, y = frame().y + 42;
    const float x = left + (right - left) * static_cast<float>(fraction());
    canvas.line(left, y, right, y, Paint::control_border, 3);
    canvas.line(left, y, x, y, Paint::accent, 3);
    canvas.round({x - 6, y - 6, 12, 12}, 6, Paint::accent, Paint::border);
}

}  // namespace wb::ui
