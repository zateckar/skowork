// policy.evaluation.export (client/evaluation Evaluation Setting): off | on, default off. Anything but exactly "on" is off.
#pragma once

#include "policy/core.h"

namespace wb::eval {

inline bool export_enabled(const policy::EffectivePolicy& p) {
    const json* v = p.find("evaluation.export");
    return v && v->is_string() && v->get<std::string>() == "on";
}

}  // namespace wb::eval
