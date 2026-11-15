#pragma once

#include <functional>
#include <memory>

#include "ui/elements.h"
#include "ui/generated_action_view.h"
#include "ui/generated_citations.h"
#include "ui/generated_surface_state.h"

namespace wb::ui {

// Until every component and Citation route is implemented, only fully supported surfaces may be composed.
bool generated_surface_presentation_supported(const json& surface, bool allow_interactions = false, bool resolve_citations = false);

class GeneratedSurfaceView : public Box {
public:
    GeneratedSurfaceView(Ui& ui, const json& surface, std::shared_ptr<GeneratedSurfaceViewState> state,
                         std::function<void()> changed = {}, GeneratedActionView::Submit submit = {},
                         GeneratedCitationResolver resolver = {}, bool accepted = false);
};

}  // namespace wb::ui
