#pragma once

#include "base/json.h"
#include "ui/elements.h"
#include "ui/generated_citations.h"

namespace wb::ui {

// A directed diagram and a readable list of the same nodes and edges.
class GeneratedDiagramView : public Box {
public:
    GeneratedDiagramView(Ui& ui, const json& component, GeneratedCitationResolver resolver = {});
};

}  // namespace wb::ui
