#pragma once

#include <functional>
#include <memory>

#include "ui/elements.h"

namespace wb::ui {

// A locally expandable section or card; the host owns the revision-bound state.
class GeneratedSectionView : public Card {
public:
    GeneratedSectionView(Ui& ui, std::string id, std::u16string title, std::shared_ptr<bool> expanded,
                         std::function<void()> changed = {})
        : Card(ui, std::move(id), std::move(title), true, !expanded || *expanded) {
        if (!expanded) expanded = std::make_shared<bool>(true);
        on_expand = [expanded = std::move(expanded), changed = std::move(changed)](bool value) {
            *expanded = value;
            auto notify_change = changed;
            if (notify_change) notify_change();
        };
    }
};

}  // namespace wb::ui
