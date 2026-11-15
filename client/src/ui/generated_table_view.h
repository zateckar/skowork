#pragma once

#include <functional>
#include <memory>
#include <vector>

#include "ui/elements.h"
#include "ui/generated_citations.h"
#include "ui/generated_table.h"

namespace wb::ui {

// A validated generated table with local exploration only. Submissions and Citation navigation are separate host routes.
class GeneratedTableView : public Box {
public:
    GeneratedTableView(Ui& ui, json table, std::shared_ptr<GeneratedTableState> state, std::function<void()> changed = {},
                       GeneratedCitationResolver resolver = {});

private:
    void refresh_rows();
    void refresh_headers();
    Ui& ui_;
    json table_;
    std::shared_ptr<GeneratedTableState> state_;
    std::function<void()> changed_;
    GeneratedCitationResolver resolver_;
    Node* rows_ = nullptr;
    std::vector<Button*> headers_;
};

}  // namespace wb::ui
