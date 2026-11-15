#pragma once

#include <functional>

#include "ui/canvas.h"

namespace wb::ui {

class GeneratedSlider : public Node {
public:
    GeneratedSlider(Ui& ui, std::string id, std::u16string label, double minimum, double maximum, double value);
    double range_value() const override { return value_; }
    double range_minimum() const override { return minimum_; }
    double range_maximum() const override { return maximum_; }
    double range_step() const override;
    bool set_range_value(double value) override;
    std::function<void(double)> on_change;
    bool on_key(const KeyEvent& event) override;
    bool on_pointer(const PointerEvent& event) override;
    void draw(Canvas& canvas) override;

private:
    double fraction() const;
    void set_fraction(double fraction);
    Ui& ui_;
    double minimum_, maximum_, value_;
};

}  // namespace wb::ui
