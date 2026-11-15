#pragma once

#include <map>
#include <memory>

#include "ui/generated_table.h"

namespace wb::ui {

struct GeneratedSurfaceViewState {
    std::map<std::string, std::shared_ptr<bool>> sections;
    std::map<std::string, std::shared_ptr<GeneratedTableState>> tables;
    std::shared_ptr<json> draft = std::make_shared<json>(json::object());
};

json generated_surface_view_state_to_json(const GeneratedSurfaceViewState& state);
GeneratedSurfaceViewState generated_surface_view_state_from_json(const json& surface, const json& saved);

}  // namespace wb::ui
