#pragma once

#include <functional>
#include <memory>
#include <vector>

#include "base/json.h"
#include "ui/elements.h"

namespace wb::ui {

// A field edits only the local draft; the host owns review and submission.
class GeneratedFieldView : public Box {
public:
    GeneratedFieldView(Ui& ui, const json& component, std::shared_ptr<json> draft, std::function<void()> changed = {});

private:
    std::vector<Checkbox*> choices_;
    bool updating_ = false;
};

}  // namespace wb::ui
