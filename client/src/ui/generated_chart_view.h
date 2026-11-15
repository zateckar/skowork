#pragma once

#include "ui/elements.h"
#include "ui/generated_citations.h"
#include "ui/generated_chart.h"

namespace wb::ui {

// The plot and its literal data alternative share the same validated chart data.
class GeneratedChartView : public Box {
public:
    GeneratedChartView(Ui& ui, const json& component, GeneratedCitationResolver resolver = {});
};

}  // namespace wb::ui
