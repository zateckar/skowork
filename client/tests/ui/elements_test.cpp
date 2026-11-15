// Tests of the native UI elements (add-native-ui tasks 2.3, 2.4): element behaviour with real DirectWrite text,
// Markdown rendering into elements, and golden images rendered off-screen on the software device.
#include <gtest/gtest.h>

#include <wincodec.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>

#include "ui/bundles.h"
#include "ui/canvas.h"
#include "ui/elements.h"
#include "ui/generated_table_view.h"
#include "ui/generated_chart_view.h"
#include "ui/generated_section_view.h"
#include "ui/generated_surface_view.h"
#include "ui/generated_surface.h"
#include "ui/generated_diagram_view.h"
#include "ui/generated_slider.h"
#include "ui/generated_field_view.h"
#include "ui/generated_action_view.h"
#include "ui/generated_citations.h"
#include "ui/list.h"
#include "ui/richtext.h"
#include "ui/text16.h"
#include "ui/textfield.h"
#include "support/golden.h"

using namespace wb;
using namespace wb::ui;

namespace {

const SystemColors kSys{0xFFFFFF, 0x000000, 0x0078D7, 0xFFFFFF, 0x0066CC, 0x6D6D6D, 0xF0F0F0, 0x000000};

struct ComInit {
    ComInit() : ok(SUCCEEDED(::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {}
    ~ComInit() {
        if (ok) ::CoUninitialize();
    }
    bool ok;
};

// An off-screen surface with the services elements need, in a fixed light theme and English.
struct Surface {
    ComInit com;
    std::unique_ptr<Renderer> r;
    std::unique_ptr<Ui> ui;
    Tree tree;
    std::unique_ptr<WindowRoot> root;
    std::vector<std::u16string> opened, copied, announced;

    Surface(UINT w, UINT h, UINT dpi = 96) {
        auto rr = Renderer::offscreen(w, h, dpi);
        EXPECT_TRUE(rr.ok());
        r = std::move(rr).value();
        auto u = Ui::create(r->dwrite(), "en");
        EXPECT_TRUE(u.ok());
        ui = std::move(u).value();
        ui->theme = make_theme(false, false, kSys);
        ui->open_link = [this](const std::u16string& s) { opened.push_back(s); };
        ui->copy_text = [this](const std::u16string& s) { copied.push_back(s); };
        ui->announce = [this](const std::u16string& s) { announced.push_back(s); };
        root = std::make_unique<WindowRoot>(tree);
    }
    Node& content() { return root->content(); }
    std::vector<uint32_t> render() {
        tree.layout(r->size_dip());
        tree.invalidate_all();
        const auto regions = r->begin_frame(tree.take_dirty());
        {
            Canvas c(*r, *ui);
            paint(tree, c, regions);
        }
        EXPECT_TRUE(r->end_frame().ok());
        auto px = r->read_pixels();
        EXPECT_TRUE(px.ok());
        return px.ok() ? *px : std::vector<uint32_t>{};
    }
    uint32_t at(const std::vector<uint32_t>& px, float x, float y) const {
        const float k = r->scale();
        return px[static_cast<size_t>(y * k) * r->width_px() + static_cast<size_t>(x * k)] & 0xFFFFFF;
    }
};

// Pixels in a rectangle that differ clearly from the background (text, borders).
size_t ink(const std::vector<uint32_t>& px, UINT w, const Rect& r, uint32_t bg) {
    size_t n = 0;
    for (int y = static_cast<int>(r.y); y < static_cast<int>(r.bottom()); ++y)
        for (int x = static_cast<int>(r.x); x < static_cast<int>(r.right()); ++x) {
            const uint32_t p = px[static_cast<size_t>(y) * w + static_cast<size_t>(x)];
            int d = 0;
            for (int s = 0; s < 24; s += 8) d += std::abs(static_cast<int>((p >> s) & 0xFF) - static_cast<int>((bg >> s) & 0xFF));
            if (d > 96) ++n;
        }
    return n;
}


}  // namespace

TEST(UiGeneratedTable, SortAndFilterKeepLocalStateAcrossRebuild) {
    Surface s(600, 500);
    const json table = json::parse(R"({"type":"table","id":"projects","label":"Projects","columns":["Name","Hours"],"rows":[["Boreal","18"],["Atlas","12"]]})");
    auto state = std::make_shared<GeneratedTableState>();
    int changes = 0;
    auto* view = s.content().emplace<GeneratedTableView>(*s.ui, table, state, [&] { ++changes; });
    auto find = [](Node& root, const std::string& id, auto&& self) -> Node* {
        if (root.automation_id() == id) return &root;
        for (const auto& child : root.children()) if (auto* result = self(*child, id, self)) return result;
        return nullptr;
    };
    auto* sort = dynamic_cast<Button*>(find(*view, "projects-sort-0", find));
    ASSERT_NE(sort, nullptr);
    s.tree.layout(s.r->size_dip());
    auto* first_row = view->children().back()->children().front().get();
    ASSERT_EQ(first_row->children().size(), 2u);
    EXPECT_FLOAT_EQ(sort->frame().x, first_row->children()[0]->frame().x);
    EXPECT_FLOAT_EQ(sort->frame().w, first_row->children()[0]->frame().w);
    EXPECT_EQ(first_row->children()[0]->name, u"Name: Boreal");
    sort->invoke();
    EXPECT_NE(sort->name.find(u"ascending"), std::u16string::npos);
    EXPECT_EQ(generated_table_rows(table, *state), (std::vector<size_t>{1, 0}));
    auto* filter = dynamic_cast<TextField*>(find(*view, "projects-filter", find));
    ASSERT_NE(filter, nullptr);
    filter->set_text(u"Atlas");
    ASSERT_TRUE(filter->on_change);
    filter->on_change();
    EXPECT_EQ(generated_table_rows(table, *state), (std::vector<size_t>{1}));
    EXPECT_EQ(changes, 2);
    s.content().clear_children();
    auto* restored = s.content().emplace<GeneratedTableView>(*s.ui, table, state, [&] { ++changes; });
    auto* restored_filter = dynamic_cast<TextField*>(find(*restored, "projects-filter", find));
    ASSERT_NE(restored_filter, nullptr);
    EXPECT_EQ(restored_filter->text(), u"Atlas");
    EXPECT_TRUE(a11y_problems(s.tree).empty());
    s.render();
}

TEST(UiGeneratedChart, ShowsDataAndUnitsAsAccessibleTextAlongsideTheVisual) {
    Surface s(600, 500);
    const json chart = json::parse(R"({"type":"bar","id":"effort","label":"Effort","unit":"hours","points":[{"label":"Atlas","value":12},{"label":"Boreal","value":-4}]})");
    auto* view = s.content().emplace<GeneratedChartView>(*s.ui, chart);
    std::vector<std::u16string> names;
    std::function<void(const Node&)> collect = [&](const Node& node) {
        if (node.role != Role::none) names.push_back(node.name);
        for (const auto& child : node.children()) collect(*child);
    };
    collect(*view);
    EXPECT_NE(std::find(names.begin(), names.end(), u"Atlas: 12 hours"), names.end());
    EXPECT_NE(std::find(names.begin(), names.end(), u"Boreal: -4 hours"), names.end());
    EXPECT_TRUE(a11y_problems(s.tree).empty());
    s.render();
}

TEST(UiGeneratedSection, ExpansionIsLocalAndSurvivesRebuild) {
    Surface s(600, 500);
    auto expanded = std::make_shared<bool>(true);
    int changes = 0;
    auto* section = s.content().emplace<GeneratedSectionView>(*s.ui, "details", u"Details", expanded, [&] { ++changes; });
    section->body().emplace<Label>(*s.ui, u"Contents");
    EXPECT_EQ(section->header().expand_state(), 1);
    section->header().set_expanded(false);
    EXPECT_FALSE(*expanded);
    EXPECT_EQ(changes, 1);
    s.content().clear_children();
    auto* restored = s.content().emplace<GeneratedSectionView>(*s.ui, "details", u"Details", expanded);
    EXPECT_EQ(restored->header().expand_state(), 0);
    EXPECT_TRUE(a11y_problems(s.tree).empty());
    s.render();
}

TEST(UiGeneratedSurface, ComposesSupportedContentAndRejectsIncompleteCatalogs) {
    Surface s(600, 500);
    const auto surface = parse_generated_surface(R"({"version":1,"id":"overview","summary":"Summary","components":[{"type":"section","id":"details","label":"Details","components":[{"type":"paragraph","id":"note","label":"Note","content":"Hello"},{"type":"table","id":"results","label":"Results","columns":["Name"],"rows":[["Atlas"]]}]}]})");
    ASSERT_TRUE(surface);
    ASSERT_TRUE(generated_surface_presentation_supported(*surface));
    auto state = std::make_shared<GeneratedSurfaceViewState>();
    int changes = 0;
    auto* view = s.content().emplace<GeneratedSurfaceView>(*s.ui, *surface, state, [&] { ++changes; });
    auto* section = dynamic_cast<GeneratedSectionView*>(view->children().front().get());
    ASSERT_NE(section, nullptr);
    EXPECT_EQ(section->body().children().size(), 2u);
    section->header().set_expanded(false);
    EXPECT_EQ(changes, 1);
    s.content().clear_children();
    auto* restored = s.content().emplace<GeneratedSurfaceView>(*s.ui, *surface, state);
    section = dynamic_cast<GeneratedSectionView*>(restored->children().front().get());
    ASSERT_NE(section, nullptr);
    EXPECT_EQ(section->header().expand_state(), 0);
    auto unsupported = *surface;
    unsupported["components"].push_back({{"type", "switch"}, {"id", "choice"}, {"label", "Choice"}, {"value", false}});
    EXPECT_FALSE(generated_surface_presentation_supported(unsupported));
    auto referenced = *surface;
    referenced["components"][0]["citations"] = json::array({"1"});
    EXPECT_FALSE(generated_surface_presentation_supported(referenced));
    EXPECT_TRUE(a11y_problems(s.tree).empty());
}

TEST(UiGeneratedSurface, ComposesFieldsOnlyWithAnExplicitHostRoute) {
    Surface s(600, 500);
    const auto surface = *parse_generated_surface(R"({"version":1,"id":"s","summary":"S","components":[{"type":"switch","id":"notify","label":"Notify","value":false},{"type":"button","id":"continue","label":"Continue"}]})");
    EXPECT_FALSE(generated_surface_presentation_supported(surface));
    EXPECT_TRUE(generated_surface_presentation_supported(surface, true));
    auto state = std::make_shared<GeneratedSurfaceViewState>();
    int local_changes = 0, host_requests = 0;
    auto* view = s.content().emplace<GeneratedSurfaceView>(*s.ui, surface, state, [&] { ++local_changes; },
        [&](std::string, std::vector<GeneratedFieldValue>, std::function<void(bool)> done) { ++host_requests; done(true); });
    ASSERT_EQ(view->children().size(), 2u);
    auto* field = dynamic_cast<GeneratedFieldView*>(view->children()[0].get());
    ASSERT_NE(field, nullptr);
    auto* toggle = dynamic_cast<Checkbox*>(field->children()[0].get());
    ASSERT_NE(toggle, nullptr);
    toggle->set_checked(true);
    EXPECT_EQ((*state->draft)["notify"], true);
    EXPECT_EQ(local_changes, 1);
    EXPECT_EQ(host_requests, 0);
    auto* action = dynamic_cast<GeneratedActionView*>(view->children()[1].get());
    ASSERT_NE(action, nullptr);
    auto* review = dynamic_cast<Button*>(action->children()[0].get());
    ASSERT_NE(review, nullptr);
    review->invoke();
    EXPECT_EQ(host_requests, 0);
    auto* panel = action->children().back().get();
    auto* confirm = dynamic_cast<Button*>(panel->children()[panel->children().size() - 2].get());
    ASSERT_NE(confirm, nullptr);
    confirm->invoke();
    EXPECT_EQ(host_requests, 1);
}

TEST(UiGeneratedSurface, ReviewRequiresControlsInExpandedSections) {
    Surface s(600, 500);
    const auto surface = *parse_generated_surface(R"({"version":1,"id":"s","summary":"S","components":[{"type":"section","id":"details","label":"Details","components":[{"type":"switch","id":"notify","label":"Notify","value":false}]},{"type":"button","id":"send","label":"Send"}]})");
    auto state = std::make_shared<GeneratedSurfaceViewState>();
    state->sections["details"] = std::make_shared<bool>(false);
    int submitted = 0;
    auto* view = s.content().emplace<GeneratedSurfaceView>(*s.ui, surface, state, std::function<void()>{},
        [&](std::string, std::vector<GeneratedFieldValue>, std::function<void(bool)>) { ++submitted; });
    auto* action = dynamic_cast<GeneratedActionView*>(view->children()[1].get());
    ASSERT_NE(action, nullptr);
    auto* review = dynamic_cast<Button*>(action->children()[0].get());
    ASSERT_NE(review, nullptr);
    review->invoke();
    ASSERT_EQ(action->children().size(), 2u);
    for (const auto& child : action->children()[1]->children()) EXPECT_NE(child->automation_id(), "send-confirm");
    EXPECT_EQ(submitted, 0);
    *state->sections["details"] = true;
    review->invoke();
    bool found = false;
    for (const auto& child : action->children()[1]->children()) if (child->automation_id() == "send-confirm") found = true;
    EXPECT_TRUE(found);
}

TEST(UiGeneratedSurface, AcceptedSurfaceShowsStatusAndDisablesEveryAction) {
    Surface s(600, 500);
    const auto surface = *parse_generated_surface(R"({"version":1,"id":"s","summary":"S","components":[{"type":"button","id":"first","label":"First"},{"type":"button","id":"second","label":"Second"}]})");
    int submitted = 0;
    auto* view = s.content().emplace<GeneratedSurfaceView>(*s.ui, surface, std::make_shared<GeneratedSurfaceViewState>(),
        std::function<void()>{}, [&](std::string, std::vector<GeneratedFieldValue>, std::function<void(bool)>) { ++submitted; },
        GeneratedCitationResolver{}, true);
    ASSERT_EQ(view->children().size(), 3u);
    EXPECT_EQ(view->children()[0]->automation_id(), "s-accepted");
    for (size_t i = 1; i < 3; ++i) {
        auto* action = dynamic_cast<GeneratedActionView*>(view->children()[i].get());
        ASSERT_NE(action, nullptr);
        auto* review = dynamic_cast<Button*>(action->children()[0].get());
        ASSERT_NE(review, nullptr);
        EXPECT_FALSE(review->enabled);
    }
    EXPECT_EQ(submitted, 0);
}

TEST(UiGeneratedSurface, ReviewControlsUseTheChosenLanguage) {
    const auto surface = *parse_generated_surface(R"({"version":1,"id":"s","summary":"S","components":[{"type":"button","id":"go","label":"Continue"}]})");
    std::vector<std::u16string> confirmations;
    for (const char* language : {"en", "cs", "de"}) {
        Surface s(400, 250);
        s.ui->strings = load_bundle(language);
        auto* view = s.content().emplace<GeneratedSurfaceView>(*s.ui, surface, std::make_shared<GeneratedSurfaceViewState>(),
            std::function<void()>{}, [](std::string, std::vector<GeneratedFieldValue>, std::function<void(bool)>) {});
        auto* action = dynamic_cast<GeneratedActionView*>(view->children()[0].get());
        ASSERT_NE(action, nullptr);
        auto* review = dynamic_cast<Button*>(action->children()[0].get());
        ASSERT_NE(review, nullptr);
        review->invoke();
        auto* panel = action->children().back().get();
        auto* confirm = dynamic_cast<Button*>(panel->children()[panel->children().size() - 2].get());
        ASSERT_NE(confirm, nullptr);
        EXPECT_EQ(confirm->name, s.ui->strings.get("generated.action.confirm"));
        confirmations.push_back(confirm->name);
    }
    EXPECT_NE(confirmations[0], confirmations[1]);
    EXPECT_NE(confirmations[0], confirmations[2]);
}

TEST(UiGeneratedCitations, KnownReferencesOpenAndUnknownReferencesArePlainText) {
    Surface s(600, 500);
    int opens = 0;
    GeneratedCitationResolver resolver = [&](const std::string& id) -> std::optional<GeneratedCitation> {
        if (id == "1") return GeneratedCitation{u"Source", [&] { ++opens; }};
        return std::nullopt;
    };
    add_generated_citations(*s.ui, s.content(), json::array({"1", "9"}), resolver, "value");
    ASSERT_EQ(s.content().children().size(), 2u);
    auto* known = dynamic_cast<Button*>(s.content().children()[0].get());
    auto* unknown = dynamic_cast<Label*>(s.content().children()[1].get());
    ASSERT_NE(known, nullptr);
    ASSERT_NE(unknown, nullptr);
    EXPECT_EQ(known->role, Role::link);
    EXPECT_EQ(unknown->role, Role::text);
    EXPECT_NE(unknown->text().find(u"9"), std::u16string::npos);
    known->invoke();
    EXPECT_EQ(opens, 1);
    EXPECT_TRUE(a11y_problems(s.tree).empty());
}

TEST(UiGeneratedCitations, TableCellReferencesAppearNextToTheirValue) {
    Surface s(600, 500);
    int opens = 0;
    GeneratedCitationResolver resolver = [&](const std::string& id) -> std::optional<GeneratedCitation> {
        if (id == "1") return GeneratedCitation{u"Source", [&] { ++opens; }};
        return std::nullopt;
    };
    const auto table = json::parse(R"({"type":"table","id":"t","label":"T","columns":["Value"],"rows":[[{"text":"X","citations":["1","9"]}]]})");
    auto* view = s.content().emplace<GeneratedTableView>(*s.ui, table, std::make_shared<GeneratedTableState>(), std::function<void()>{}, resolver);
    auto* cell = view->children().back()->children()[0]->children()[0].get();
    ASSERT_EQ(cell->children().size(), 3u);
    EXPECT_EQ(cell->children()[0]->name, u"Value: X");
    EXPECT_EQ(cell->children()[1]->role, Role::link);
    EXPECT_EQ(cell->children()[2]->role, Role::text);
    cell->children()[1]->invoke();
    EXPECT_EQ(opens, 1);
    EXPECT_TRUE(a11y_problems(s.tree).empty());
}

TEST(UiGeneratedCitations, ChartPointAndDiagramEdgeKeepTheirReferenceContext) {
    Surface s(600, 500);
    GeneratedCitationResolver resolver = [](const std::string& id) -> std::optional<GeneratedCitation> {
        if (id == "1") return GeneratedCitation{u"Source", [] {}};
        return std::nullopt;
    };
    const auto chart = json::parse(R"({"type":"bar","id":"c","label":"C","unit":"hours","points":[{"label":"A","value":1,"citations":["1"]}]})");
    auto* chart_view = s.content().emplace<GeneratedChartView>(*s.ui, chart, resolver);
    auto* point = chart_view->children().back().get();
    ASSERT_EQ(point->children().size(), 2u);
    EXPECT_EQ(point->children()[0]->name, u"A: 1 hours");
    EXPECT_EQ(point->children()[1]->role, Role::link);
    const auto diagram = json::parse(R"({"type":"diagram","id":"d","label":"D","nodes":[{"id":"a","label":"A"},{"id":"b","label":"B"}],"edges":[{"from":"a","to":"b","citations":["9"]}]})");
    auto* diagram_view = s.content().emplace<GeneratedDiagramView>(*s.ui, diagram, resolver);
    auto* edge = diagram_view->children().back().get();
    ASSERT_EQ(edge->children().size(), 2u);
    EXPECT_EQ(edge->children()[0]->name, u"A → B");
    EXPECT_EQ(edge->children()[1]->role, Role::text);
    EXPECT_NE(edge->children()[1]->name.find(u"9"), std::u16string::npos);
}

TEST(UiGeneratedCitations, SurfaceRequiresResolverBeforeShowingReferencedContent) {
    Surface s(600, 500);
    const auto surface = *parse_generated_surface(R"({"version":1,"id":"s","summary":"S","components":[{"type":"paragraph","id":"p","label":"Fact","content":"Value","citations":["1"]}]})");
    EXPECT_FALSE(generated_surface_presentation_supported(surface));
    EXPECT_TRUE(generated_surface_presentation_supported(surface, false, true));
    GeneratedCitationResolver resolver = [](const std::string&) -> std::optional<GeneratedCitation> {
        return GeneratedCitation{u"Source", [] {}};
    };
    auto* view = s.content().emplace<GeneratedSurfaceView>(*s.ui, surface, std::make_shared<GeneratedSurfaceViewState>(),
        std::function<void()>{}, GeneratedActionView::Submit{}, resolver);
    ASSERT_EQ(view->children().size(), 1u);
    const auto& content = view->children()[0]->children();
    ASSERT_EQ(content.size(), 3u);
    EXPECT_EQ(content[1]->name, u"Value");
    EXPECT_EQ(content[2]->role, Role::link);
}

TEST(UiGeneratedDiagram, CyclesAndSelfEdgesHaveReadableAlternatives) {
    Surface s(600, 500);
    const auto diagram = json::parse(R"({"type":"diagram","id":"flow","label":"Flow","nodes":[{"id":"a","label":"Alpha"},{"id":"b","label":"Beta"}],"edges":[{"from":"a","to":"b"},{"from":"b","to":"a"},{"from":"a","to":"a"}]})");
    auto* view = s.content().emplace<GeneratedDiagramView>(*s.ui, diagram);
    std::vector<std::u16string> names;
    std::function<void(const Node&)> collect = [&](const Node& node) {
        if (node.role != Role::none) names.push_back(node.name);
        for (const auto& child : node.children()) collect(*child);
    };
    collect(*view);
    EXPECT_NE(std::find(names.begin(), names.end(), u"Alpha → Beta"), names.end());
    EXPECT_NE(std::find(names.begin(), names.end(), u"Beta → Alpha"), names.end());
    EXPECT_NE(std::find(names.begin(), names.end(), u"Alpha → Alpha"), names.end());
    EXPECT_TRUE(a11y_problems(s.tree).empty());
    s.render();
}

TEST(UiGeneratedSlider, ChangesAreLocalAndKeyboardAccessible) {
    Surface s(600, 500);
    auto* slider = s.content().emplace<GeneratedSlider>(*s.ui, "amount", u"Amount", 0, 10, 5);
    int changes = 0;
    slider->on_change = [&](double) { ++changes; };
    s.tree.layout(s.r->size_dip());
    s.tree.set_focus(slider);
    EXPECT_TRUE(s.tree.key({kKeyRight}));
    EXPECT_GT(slider->range_value(), 5);
    EXPECT_EQ(changes, 1);
    EXPECT_FALSE(slider->set_range_value(11));
    EXPECT_EQ(changes, 1);
    EXPECT_TRUE(slider->set_range_value(3));
    EXPECT_DOUBLE_EQ(slider->range_value(), 3);
    EXPECT_EQ(changes, 2);
    EXPECT_FALSE(slider->on_pointer({PointerEvent::down, 10, 10, 0, 1}));
    EXPECT_DOUBLE_EQ(slider->range_value(), 3);
    EXPECT_TRUE(a11y_problems(s.tree).empty());
    s.render();
}

TEST(UiGeneratedField, SelectionSwitchAndTextOnlyChangeTheLocalDraft) {
    Surface s(600, 500);
    auto draft = std::make_shared<json>(json::object());
    int changes = 0;
    auto changed = [&] { ++changes; };
    auto* choice = s.content().emplace<GeneratedFieldView>(*s.ui, json::parse(R"({"type":"select","id":"frequency","label":"Frequency","options":["Weekly","Monthly"]})"), draft, changed);
    auto* first = dynamic_cast<Checkbox*>(choice->children()[1].get());
    auto* second = dynamic_cast<Checkbox*>(choice->children()[2].get());
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    first->set_checked(true);
    EXPECT_EQ((*draft)["frequency"], "Weekly");
    second->set_checked(true);
    EXPECT_FALSE(first->checked());
    EXPECT_EQ((*draft)["frequency"], "Monthly");
    auto* flag = s.content().emplace<GeneratedFieldView>(*s.ui, json::parse(R"({"type":"switch","id":"notify","label":"Notify","value":false})"), draft, changed);
    auto* toggle = dynamic_cast<Checkbox*>(flag->children().front().get());
    ASSERT_NE(toggle, nullptr);
    toggle->set_checked(true);
    EXPECT_EQ((*draft)["notify"], true);
    auto* note = s.content().emplace<GeneratedFieldView>(*s.ui, json::parse(R"({"type":"text","id":"note","label":"Note","value":""})"), draft, changed);
    auto* text = dynamic_cast<TextField*>(note->children().back().get());
    ASSERT_NE(text, nullptr);
    text->set_text(u"Ready");
    text->on_change();
    EXPECT_EQ((*draft)["note"], "Ready");
    EXPECT_EQ(changes, 4);
    EXPECT_TRUE(a11y_problems(s.tree).empty());
    s.render();
}

TEST(UiGeneratedAction, RequiresReviewAndRejectsValuesChangedAfterReview) {
    Surface s(600, 500);
    const auto surface = *parse_generated_surface(R"({"version":1,"id":"s","summary":"S","components":[{"type":"switch","id":"notify","label":"Notify","value":false},{"type":"button","id":"continue","label":"Continue"}]})");
    auto draft = std::make_shared<json>(json{{"notify", true}});
    int submissions = 0;
    std::function<void(bool)> finish;
    auto* action = s.content().emplace<GeneratedActionView>(*s.ui, surface["components"][1], surface, draft,
        [&](const std::string&, const std::vector<GeneratedFieldValue>&, std::function<void(bool)> done) {
            ++submissions;
            finish = std::move(done);
        });
    auto* review = dynamic_cast<Button*>(action->children().front().get());
    ASSERT_NE(review, nullptr);
    review->invoke();
    EXPECT_EQ(submissions, 0);
    ASSERT_EQ(action->children().size(), 2u);
    auto* panel = action->children().back().get();
    auto* confirm = dynamic_cast<Button*>(panel->children()[panel->children().size() - 2].get());
    ASSERT_NE(confirm, nullptr);
    (*draft)["notify"] = false;
    confirm->invoke();
    EXPECT_EQ(submissions, 0);
    auto* status = dynamic_cast<Label*>(panel->children()[panel->children().size() - 3].get());
    ASSERT_NE(status, nullptr);
    EXPECT_EQ(status->automation_id(), "continue-status");
    EXPECT_TRUE(status->visible);
    EXPECT_EQ(status->text(), u"The fields changed. Review them again before confirming.");
    review->invoke();
    panel = action->children().back().get();
    confirm = dynamic_cast<Button*>(panel->children()[panel->children().size() - 2].get());
    ASSERT_NE(confirm, nullptr);
    confirm->invoke();
    EXPECT_EQ(submissions, 1);
    EXPECT_FALSE(confirm->enabled);
    confirm->invoke();
    EXPECT_EQ(submissions, 1);
    ASSERT_TRUE(finish);
    finish(false);
    EXPECT_TRUE(confirm->enabled);  // a rejected host request can be retried
    confirm->invoke();
    EXPECT_EQ(submissions, 2);
    finish(true);
    confirm->invoke();
    EXPECT_EQ(submissions, 2);
    EXPECT_TRUE(a11y_problems(s.tree).empty());
}

// --- elements ---------------------------------------------------------------------------------------------------

TEST(UiElements, ButtonsCheckboxAndCardBehave) {
    Surface s(400, 300);
    Node& c = s.content();
    c.padding = Insets::all(10);
    c.gap = 8;
    auto* b = c.emplace<Button>(*s.ui, "save", u"Save", Button::Kind::primary);
    int clicks = 0;
    b->on_click = [&] { ++clicks; };
    auto* icon = c.emplace<IconButton>(*s.ui, "more", icon::more, u"More options");
    auto* check = c.emplace<Checkbox>(*s.ui, "notify", u"Notify me");
    bool last = false;
    check->on_change = [&](bool on) { last = on; };
    auto* card = c.emplace<Card>(*s.ui, "card", u"Tool call: files.read", true, false);
    card->body().emplace<Label>(*s.ui, u"Arguments", TextStyle::body);
    s.tree.layout(s.r->size_dip());

    EXPECT_GT(b->frame().w, 40.f);  // fits its label
    EXPECT_FLOAT_EQ(b->frame().h, 32.f);
    EXPECT_FLOAT_EQ(icon->frame().w, 32.f);
    EXPECT_EQ(icon->help, u"More options");  // tooltip
    EXPECT_TRUE(a11y_problems(s.tree).empty());

    s.tree.pointer({PointerEvent::down, b->frame().x + 5, b->frame().y + 5});
    s.tree.pointer({PointerEvent::up, b->frame().x + 5, b->frame().y + 5});
    EXPECT_EQ(clicks, 1);
    s.tree.set_focus(check);
    s.tree.key({kKeySpace});
    EXPECT_TRUE(last);
    EXPECT_EQ(check->toggle_state(), 1);

    // The card starts collapsed; its header is an expand/collapse button.
    EXPECT_FALSE(card->body().visible);
    const float collapsed_h = card->frame().h;
    EXPECT_EQ(card->header().expand_state(), 0);
    s.tree.set_focus(&card->header());
    s.tree.key({kKeyEnter});
    s.tree.layout_if_needed();
    EXPECT_EQ(card->header().expand_state(), 1);
    EXPECT_GT(card->frame().h, collapsed_h);
}

TEST(UiElements, LabelsWrapAndEllipsize) {
    Surface s(300, 200);
    const std::u16string text(200, u'x');
    auto* wrap = s.content().emplace<Label>(*s.ui, text, TextStyle::body);
    auto* one = s.content().emplace<Label>(*s.ui, text, TextStyle::body, Paint::text, false);
    s.tree.layout(s.r->size_dip());
    EXPECT_GT(wrap->frame().h, 60.f);  // several lines
    EXPECT_LT(one->frame().h, 25.f);   // one line, cut to the width
    EXPECT_FLOAT_EQ(one->frame().w, 300.f);
    EXPECT_GT(s.ui->texts().size(), 0u);
}

TEST(UiElements, MenuOpensBelowItsAnchorAndClosesWithEscapeRestoringFocus) {
    Surface s(400, 300);
    auto* opener = s.content().emplace<IconButton>(*s.ui, "more", icon::more, u"More options");
    s.tree.layout(s.r->size_dip());
    s.tree.set_focus(opener);
    int archived = 0;
    auto menu = std::make_unique<Menu>("menu", u"Thread actions");
    menu->add_item(*s.ui, "archive", u"Archive", [&] { ++archived; }, icon::archive);
    menu->add_item(*s.ui, "delete", u"Delete", [] {}, icon::del);
    Menu* m = menu.get();
    s.root->overlay().show(std::move(menu), opener->frame(), true);
    s.tree.layout_if_needed();
    EXPECT_GE(m->frame().y, opener->frame().bottom());
    ASSERT_NE(s.tree.focused(), nullptr);
    EXPECT_EQ(s.tree.focused()->automation_id(), "archive");
    s.tree.key({kKeyDown});
    EXPECT_EQ(s.tree.focused()->automation_id(), "delete");
    s.tree.key({kKeyEscape});
    EXPECT_FALSE(s.root->overlay().has_popup());
    EXPECT_EQ(s.tree.focused(), opener);
    // Reopen, choose with Enter: the action runs and the menu closes.
    menu = std::make_unique<Menu>("menu", u"Thread actions");
    menu->add_item(*s.ui, "archive", u"Archive", [&] { ++archived; });
    s.root->overlay().show(std::move(menu), opener->frame(), true);
    s.tree.layout_if_needed();
    s.tree.key({kKeyEnter});
    EXPECT_EQ(archived, 1);
    EXPECT_FALSE(s.root->overlay().has_popup());
    // A press outside a popup closes it.
    menu = std::make_unique<Menu>("menu", u"Thread actions");
    menu->add_item(*s.ui, "archive", u"Archive", [] {});
    s.root->overlay().show(std::move(menu), opener->frame(), false);
    s.tree.layout_if_needed();
    s.tree.pointer({PointerEvent::down, 390, 290});
    EXPECT_FALSE(s.root->overlay().has_popup());
}

TEST(UiElements, SplitterResizesItsPaneByDragAndKeyboard) {
    Surface s(600, 300);
    Node& c = s.content();
    c.axis = Axis::row;
    c.height = Sizing::fill;
    auto* left = c.emplace<Box>("left");
    left->width = Sizing::fixed;
    left->fixed_w = 200;
    left->height = Sizing::fill;
    auto* split = c.emplace<Splitter>("split", u"Resize the thread list", *left, 150, 400);
    c.emplace<Box>("right")->height = Sizing::fill;
    s.tree.layout(s.r->size_dip());
    const float x = split->frame().x + 2;
    s.tree.pointer({PointerEvent::down, x, 100});
    s.tree.pointer({PointerEvent::move, x + 50, 100});
    s.tree.pointer({PointerEvent::up, x + 50, 100});
    EXPECT_FLOAT_EQ(left->fixed_w, 250);
    s.tree.set_focus(split);
    s.tree.key({kKeyRight});
    EXPECT_FLOAT_EQ(left->fixed_w, 266);
    s.tree.key({kKeyRight, false, true});
    s.tree.key({kKeyRight, false, true});
    s.tree.key({kKeyRight, false, true});
    EXPECT_FLOAT_EQ(left->fixed_w, 400);  // clamped
    s.tree.layout_if_needed();
    EXPECT_FLOAT_EQ(split->frame().x, 400);
}

TEST(UiElements, ImageDecodesScalesAndNeverUpscales) {
    Surface s(400, 300);
    // A 2x1 PNG made by WIC itself.
    const auto file = std::filesystem::temp_directory_path() / "skowork-ui-test.png";
    ASSERT_TRUE(write_png(file, {0xFFFF0000, 0xFF0000FF}, 2, 1));
    std::ifstream in(file, std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    std::filesystem::remove(file);
    auto img = decode_image(bytes.data(), bytes.size());
    ASSERT_TRUE(img.ok()) << img.error().message;
    auto* view = s.content().emplace<ImageView>(u"two pixels", *img);
    s.tree.layout(s.r->size_dip());
    EXPECT_FLOAT_EQ(view->frame().w, 2);
    EXPECT_FLOAT_EQ(view->frame().h, 1);
    EXPECT_FALSE(decode_image("not an image", 12).ok());
    EXPECT_FALSE(decode_image(nullptr, 0).ok());
}

// --- Markdown ---------------------------------------------------------------------------------------------------

TEST(UiMarkdown, BlocksBecomeElementsWithLinksAndCopy) {
    Surface s(600, 800);
    auto* md = s.content().emplace<MarkdownView>(*s.ui, u"Answer");
    md->set_markdown(
        "# Plan\n\nRead the [guide](https://example.com/guide) and `run`.\n\n- one\n- two\n\n```sh\nmake all\n```\n\n"
        "| A | B |\n|---|---|\n| 1 | 2 |\n\n> quoted\n\n---\n\n![diagram](https://tracker.example/x.png) <script>x</script>\n");
    s.tree.layout(s.r->size_dip());
    EXPECT_EQ(md->blocks().size(), 9u);
    EXPECT_EQ(md->children().size(), 9u);
    EXPECT_EQ(md->value().find(u"# Plan"), std::u16string::npos);  // markup is not shown
    EXPECT_NE(md->value().find(u"<script>x</script>"), std::u16string::npos);
    EXPECT_TRUE(a11y_problems(s.tree).empty()) << a11y_problems(s.tree).front();

    // Links are focusable children placed over their text; the image is a link, never an image element.
    std::vector<LinkSpan*> links;
    std::function<void(Node&)> walk = [&](Node& n) {
        if (auto* l = dynamic_cast<LinkSpan*>(&n)) links.push_back(l);
        EXPECT_NE(n.role, Role::image);
        for (auto& c : n.children()) walk(*c);
    };
    walk(*md);
    ASSERT_EQ(links.size(), 2u);
    EXPECT_EQ(links[0]->url(), u"https://example.com/guide");
    EXPECT_EQ(links[0]->name, u"guide");
    EXPECT_EQ(links[1]->url(), u"https://tracker.example/x.png");
    EXPECT_FALSE(links[0]->frame().empty());
    EXPECT_GT(links[0]->frame().x, md->frame().x + 20);  // not at the start of the line
    s.tree.pointer({PointerEvent::down, links[0]->frame().x + 2, links[0]->frame().y + 2});
    s.tree.pointer({PointerEvent::up, links[0]->frame().x + 2, links[0]->frame().y + 2});
    EXPECT_EQ(s.opened, (std::vector<std::u16string>{u"https://example.com/guide"}));
    s.tree.set_focus(links[1]);
    s.tree.key({kKeyEnter});
    EXPECT_EQ(s.opened.size(), 2u);  // the window's open_link shows the target and asks before opening

    // The code block's Copy button copies the code and announces it.
    Node* copy = nullptr;
    for (Node* f : s.tree.focus_chain())
        if (f->automation_id().ends_with("-copy")) copy = f;
    ASSERT_NE(copy, nullptr);
    EXPECT_EQ(copy->name, u"Copy code");
    copy->invoke();
    EXPECT_EQ(s.copied, (std::vector<std::u16string>{u"make all"}));
    EXPECT_EQ(s.announced.size(), 1u);
}

// client/work-inbox Message Text Selection (improve-desktop-look 1.3): a drag selects words, and across blocks in
// document order; a click without a drag clears it; one selection per window.
TEST(UiMarkdown, DragSelectsTextAcrossBlocksAndAClickClears) {
    Surface s(600, 600);
    auto* md = s.content().emplace<MarkdownView>(*s.ui, u"Message");
    md->set_markdown("Alpha beta gamma delta.\n\n- one two\n- three\n");
    auto* other = s.content().emplace<MarkdownView>(*s.ui, u"Other");
    other->set_markdown("Another message.");
    s.tree.layout(s.r->size_dip());
    std::vector<RichText*> texts;
    std::function<void(Node&)> walk = [&](Node& n) {
        if (auto* r = dynamic_cast<RichText*>(&n)) texts.push_back(r);
        for (auto& c : n.children()) walk(*c);
    };
    walk(*md);
    ASSERT_EQ(texts.size(), 3u);
    RichText* para = texts[0];
    const float y = para->frame().y + para->frame().h / 2;
    // The x where a text position starts.
    const auto x_of = [&](uint32_t pos) {
        for (float x = para->frame().x; x < para->frame().right(); x += 0.5f)
            if (para->position_at(x, y) >= pos) return x;
        return para->frame().right();
    };
    EXPECT_EQ(para->cursor(), 3);  // the text cursor

    // Two words: "beta gamma".
    s.tree.pointer({PointerEvent::down, x_of(6) + 0.2f, y});
    s.tree.pointer({PointerEvent::move, x_of(16) + 0.2f, y});
    s.tree.pointer({PointerEvent::up, x_of(16) + 0.2f, y});
    EXPECT_EQ(md->selected_text(), u"beta gamma");
    EXPECT_EQ(s.ui->selection, md);

    // From the start of the paragraph to the end of the last list item, across three blocks.
    RichText* last = texts[2];
    s.tree.pointer({PointerEvent::down, para->frame().x + 0.5f, y});
    s.tree.pointer({PointerEvent::move, last->frame().right() - 1, last->frame().y + last->frame().h / 2});
    s.tree.pointer({PointerEvent::up, last->frame().right() - 1, last->frame().y + last->frame().h / 2});
    EXPECT_EQ(md->selected_text(), u"Alpha beta gamma delta.\none two\nthree");

    // Selecting in another message clears the first: one selection per window.
    RichText* o = dynamic_cast<RichText*>(other->children().front().get());
    ASSERT_NE(o, nullptr);
    const float oy = o->frame().y + o->frame().h / 2;
    s.tree.pointer({PointerEvent::down, o->frame().x + 0.5f, oy});
    s.tree.pointer({PointerEvent::move, o->frame().x + 40, oy});
    s.tree.pointer({PointerEvent::up, o->frame().x + 40, oy});
    EXPECT_EQ(s.ui->selection, other);
    EXPECT_TRUE(md->selected_text().empty());
    EXPECT_FALSE(other->selected_text().empty());

    // A click without a drag selects nothing.
    s.tree.pointer({PointerEvent::down, o->frame().x + 10, oy});
    s.tree.pointer({PointerEvent::up, o->frame().x + 10, oy});
    EXPECT_TRUE(other->selected_text().empty());
    EXPECT_EQ(s.ui->selection, nullptr);

    // A view that goes away gives up the selection.
    s.tree.pointer({PointerEvent::down, o->frame().x + 0.5f, oy});
    s.tree.pointer({PointerEvent::move, o->frame().x + 40, oy});
    s.tree.pointer({PointerEvent::up, o->frame().x + 40, oy});
    s.content().remove(other);
    EXPECT_EQ(s.ui->selection, nullptr);
}

TEST(UiMarkdown, StreamingRebuildsOnlyTheChangedTail) {
    Surface s(600, 600);
    auto* md = s.content().emplace<MarkdownView>(*s.ui, u"Answer");
    std::string text = "First paragraph.\n\nSecond";
    md->set_markdown(text);
    EXPECT_EQ(md->rebuilt_last(), 2u);
    Node* first = md->children().front().get();
    text += " paragraph, still streaming";
    md->set_markdown(text);
    EXPECT_EQ(md->rebuilt_last(), 1u);
    EXPECT_EQ(md->children().front().get(), first);  // the finished block kept its element
    text += "\n\n```py\nprint(1)";
    md->set_markdown(text);
    EXPECT_EQ(md->rebuilt_last(), 1u);
    EXPECT_EQ(md->children().size(), 3u);
    s.tree.layout(s.r->size_dip());
    // Each prefix of a long answer lays out without trouble.
    const std::string full = "## Steps\n\n1. **Open** the file\n2. Run `make`\n\n| x | y |\n|---|---|\n| 1 | 2 |\n";
    for (size_t n = 0; n <= full.size(); n += 3) {
        md->set_markdown(std::string_view(full).substr(0, n));
        s.tree.layout(s.r->size_dip());
    }
}

// --- golden images ------------------------------------------------------------------------------------------------

TEST(UiGolden, ElementGallery) {
    if (!reference_fonts()) GTEST_SKIP() << "reference fonts (Segoe UI Variable, Segoe Fluent Icons) not installed";
    Surface s(480, 360);
    Node& c = s.content();
    c.padding = Insets::all(16);
    c.gap = 12;
    auto* row = c.emplace<Node>();
    row->axis = Axis::row;
    row->gap = 8;
    row->emplace<Button>(*s.ui, "primary", u"Approve", Button::Kind::primary, icon::check);
    row->emplace<Button>(*s.ui, "standard", u"Deny");
    row->emplace<IconButton>(*s.ui, "more", icon::more, u"More options", Button::Kind::standard);
    row->emplace<Badge>(*s.ui, u"3");
    c.emplace<Checkbox>(*s.ui, "check", u"Remember this choice", true);
    auto* card = c.emplace<Card>(*s.ui, "card", u"files.read", true, true);
    card->body().emplace<Label>(*s.ui, u"C:\\Users\\Example\\report.docx", TextStyle::code, Paint::text_secondary);
    auto* md = c.emplace<MarkdownView>(*s.ui, u"Answer");
    md->set_markdown("**Done.** See the [report](https://example.com) and `notes.txt`:\n\n- first\n- second\n");
    const auto px = s.render();
    // Sanity independent of the reference: the primary button is filled with the brand accent (Emerald in light).
    Node* primary = s.tree.focus_chain().front();
    EXPECT_EQ(s.at(px, primary->frame().x + 3, primary->frame().y + primary->frame().h / 2), 0x0E3A2Fu);
    expect_golden("element_gallery", px, s.r->width_px(), s.r->height_px());
}

TEST(UiGolden, MarkdownDarkHighDpi) {
    if (!reference_fonts()) GTEST_SKIP() << "reference fonts (Segoe UI Variable, Segoe Fluent Icons) not installed";
    Surface s(720, 540, 144);  // 150 %
    s.ui->theme = make_theme(true, false, kSys);
    Node& c = s.content();
    c.padding = Insets::all(12);
    auto* md = c.emplace<MarkdownView>(*s.ui, u"Answer");
    md->set_markdown(
        "## Result\n\nThe build **passed** with ~~two~~ no warnings.\n\n```cpp\nint main() { return 0; }\n```\n\n"
        "| Test | Time |\n|---|---|\n| ui_tests | 4 ms |\n\n> Note: *quoted*\n");
    const auto px = s.render();
    EXPECT_EQ(s.at(px, 2, 2), 0x131A17u);  // dark background
    EXPECT_GT(ink(px, s.r->width_px(), {0, 0, 720, 540}, 0x131A17), 2000u);
    expect_golden("markdown_dark_150", px, s.r->width_px(), s.r->height_px());
}

TEST(UiGolden, HighContrastUsesSystemColours) {
    Surface s(240, 80);
    const SystemColors hc{0x000000, 0xFFFFFF, 0x1AEBFF, 0x000000, 0xFFFF00, 0x3FF23F, 0x000000, 0xFFFFFF};
    s.ui->theme = make_theme(true, true, hc);
    s.content().padding = Insets::all(8);
    auto* b = s.content().emplace<Button>(*s.ui, "ok", u"OK", Button::Kind::primary);
    const auto px = s.render();
    EXPECT_EQ(s.at(px, 1, 1), 0x000000u);
    EXPECT_EQ(s.at(px, b->frame().x + 4, b->frame().y + b->frame().h / 2), 0x1AEBFFu);  // highlight, not the accent
}

TEST(UiGeneratedSurface, HighContrastUsesSystemColoursAndNamedControls) {
    Surface s(600, 500);
    const SystemColors hc{0x000000, 0xFFFFFF, 0x1AEBFF, 0x000000, 0xFFFF00, 0x3FF23F, 0x000000, 0xFFFFFF};
    s.ui->theme = make_theme(true, true, hc);
    s.content().padding = Insets::all(8);
    const auto surface = parse_generated_surface(R"({"version":1,"id":"s","summary":"Summary","components":[{"type":"bar","id":"chart","label":"Cost","unit":"EUR","points":[{"label":"A","value":4}]},{"type":"switch","id":"notify","label":"Notify","value":false},{"type":"button","id":"go","label":"Continue"}]})");
    ASSERT_TRUE(surface);
    auto* view = s.content().emplace<GeneratedSurfaceView>(*s.ui, *surface, std::make_shared<GeneratedSurfaceViewState>(),
        std::function<void()>{}, [](std::string, std::vector<GeneratedFieldValue>, std::function<void(bool)>) {});
    const auto px = s.render();
    EXPECT_EQ(s.at(px, 1, 1), 0x000000u);
    EXPECT_TRUE(a11y_problems(s.tree).empty());
    ASSERT_EQ(view->children().size(), 3u);
    EXPECT_EQ(view->children()[0]->name, u"Cost");
    EXPECT_EQ(view->children()[1]->name, u"Notify");
    auto* action = dynamic_cast<GeneratedActionView*>(view->children()[2].get());
    ASSERT_NE(action, nullptr);
    auto* review = dynamic_cast<Button*>(action->children()[0].get());
    ASSERT_NE(review, nullptr);
    EXPECT_EQ(s.at(px, review->frame().x + 4, review->frame().y + review->frame().h / 2), 0x1AEBFFu);
}

// Ctrl+C copies the message selection while the selected view has keyboard focus; a press elsewhere moves the focus
// and clears the selection (design D8).
TEST(UiMarkdown, CtrlCCopiesTheSelectedWordsAndAPressElsewhereClearsThem) {
    Surface s(600, 600);
    std::vector<std::u16string> copied;
    s.ui->copy_text = [&copied](const std::u16string& t) { copied.push_back(t); };
    auto* md = s.content().emplace<MarkdownView>(*s.ui, u"Message");
    md->set_markdown("Alpha beta gamma delta.\n\n- one two\n- three\n");
    auto* other = s.content().emplace<Button>(*s.ui, "other-action", u"Other");
    s.tree.layout(s.r->size_dip());
    std::vector<RichText*> texts;
    std::function<void(Node&)> walk = [&](Node& n) {
        if (auto* r = dynamic_cast<RichText*>(&n)) texts.push_back(r);
        for (auto& c : n.children()) walk(*c);
    };
    walk(*md);
    ASSERT_EQ(texts.size(), 3u);
    RichText* para = texts[0];
    const float y = para->frame().y + para->frame().h / 2;
    const auto x_of = [&](uint32_t pos) {
        for (float x = para->frame().x; x < para->frame().right(); x += 0.5f)
            if (para->position_at(x, y) >= pos) return x;
        return para->frame().right();
    };
    // A press and drag over "beta gamma" gives the view keyboard focus.
    s.tree.pointer({PointerEvent::down, x_of(6) + 0.2f, y});
    s.tree.pointer({PointerEvent::move, x_of(16) + 0.2f, y});
    s.tree.pointer({PointerEvent::up, x_of(16) + 0.2f, y});
    EXPECT_EQ(s.tree.focused(), md);
    const KeyEvent ctrl_c{kKeyC, false, true, false};
    EXPECT_TRUE(copy_message_selection(*s.ui, ctrl_c));
    ASSERT_EQ(copied.size(), 1u);
    EXPECT_EQ(copied[0], u"beta gamma");
    EXPECT_FALSE(copy_message_selection(*s.ui, KeyEvent{kKeyC, true, true, false}));  // Ctrl+Shift+C is not a copy here
    EXPECT_FALSE(copy_message_selection(*s.ui, KeyEvent{0x56, false, true, false}));  // Ctrl+V

    // A press on another control moves the focus there and clears the selection.
    const float bx = other->frame().x + other->frame().w / 2, by = other->frame().y + other->frame().h / 2;
    s.tree.pointer({PointerEvent::down, bx, by});
    s.tree.pointer({PointerEvent::up, bx, by});
    EXPECT_EQ(s.tree.focused(), other);
    EXPECT_EQ(s.ui->selection, nullptr);
    EXPECT_TRUE(md->selected_text().empty());
    EXPECT_FALSE(copy_message_selection(*s.ui, ctrl_c));
    EXPECT_EQ(copied.size(), 1u);
}
