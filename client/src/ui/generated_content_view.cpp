#include "ui/generated_content_view.h"

#include <string>

#include "ui/richtext.h"
#include "ui/text16.h"

namespace wb::ui {
namespace {

// Segoe Fluent Icons used only here.
constexpr char16_t kRing = 0xEA3A, kCompleted = 0xE930, kDocument = 0xE8A5, kFlag = 0xE7C1, kPeople = 0xE716;

std::u16string field(const json& j, const char* key) { return to_utf16(jstr(j, key)); }

// A component's frame: a group named by its label, with the label as a heading.
Box* frame_of(Ui& ui, Node& parent, const json& c) {
    auto* box = parent.emplace<Box>(jstr(c, "id"));
    box->role = Role::group;
    box->name = field(c, "label");
    box->gap = 4;
    auto* heading = box->emplace<Label>(ui, box->name, TextStyle::body_strong, Paint::text, true, box->automation_id() + "-label");
    heading->role = Role::heading;
    return box;
}

void citations_of(Ui& ui, Node& parent, const json& j, const GeneratedCitationResolver& resolver, const std::string& prefix) {
    if (j.contains("citations")) add_generated_citations(ui, parent, j["citations"], resolver, prefix);
}

Label* glyph(Node& parent, Ui& ui, char16_t g, Paint color) {
    auto* l = parent.emplace<Label>(ui, std::u16string(1, g), TextStyle::icon, color, false);
    l->role = Role::none;  // decorative: the row's name says the same
    return l;
}

Box* list_of(Node& parent, const std::string& id, const std::u16string& name) {
    auto* list = parent.emplace<Box>(id + "-items");
    list->role = Role::list;
    list->name = name;
    list->gap = 6;
    return list;
}

Box* row_of(Node& list, const std::string& id, size_t i) {
    auto* row = list.emplace<Box>(id + "-item-" + std::to_string(i));
    row->axis = Axis::row;
    row->gap = 10;
    row->role = Role::list_item;
    return row;
}

void markdown(Ui& ui, Node& parent, const json& c, const GeneratedCitationResolver& resolver) {
    auto* box = frame_of(ui, parent, c);
    box->emplace<MarkdownView>(ui, box->name, box->automation_id() + "-content")->set_markdown(jstr(c, "content"));
    citations_of(ui, *box, c, resolver, box->automation_id());
}

void facts(Ui& ui, Node& parent, const json& c, const GeneratedCitationResolver& resolver) {
    auto* box = frame_of(ui, parent, c);
    citations_of(ui, *box, c, resolver, box->automation_id());
    auto* list = list_of(*box, box->automation_id(), box->name);
    list->gap = 4;
    const auto& items = c["items"];
    for (size_t i = 0; i < items.size(); ++i) {
        const auto label = field(items[i], "label"), value = field(items[i], "value");
        auto* row = row_of(*list, box->automation_id(), i);
        row->gap = 12;
        row->name = label + u": " + value;
        auto* l = row->emplace<Label>(ui, label, TextStyle::body, Paint::text_secondary, true);
        l->width = Sizing::fixed;
        l->fixed_w = 140;
        auto* cell = row->emplace<Box>();
        cell->emplace<Label>(ui, value, TextStyle::body, Paint::text, true);
        citations_of(ui, *cell, items[i], resolver, row->automation_id());
    }
}

void callout(Ui& ui, Node& parent, const json& c, const GeneratedCitationResolver& resolver) {
    const auto tone = jstr(c, "tone");
    Paint fill = Paint::info_background, color = Paint::info;
    char16_t g = icon::info;
    if (tone == "success") fill = Paint::accent_surface, color = Paint::success, g = kCompleted;
    else if (tone == "warning") fill = Paint::warning_background, color = Paint::warning, g = icon::warning;
    else if (tone == "danger") fill = Paint::error_background, color = Paint::error, g = icon::error;
    auto* box = parent.emplace<Box>(jstr(c, "id"), fill, 6);
    box->axis = Axis::row;
    box->padding = Insets::all(10);
    box->gap = 10;
    box->role = Role::group;
    box->name = ui.strings.get("generated.callout." + (tone.empty() ? std::string("info") : tone)) + u": " + field(c, "label");
    glyph(*box, ui, g, color);
    auto* body = box->emplace<Box>();
    body->gap = 2;
    auto* heading = body->emplace<Label>(ui, field(c, "label"), TextStyle::body_strong, Paint::text, true, box->automation_id() + "-label");
    heading->role = Role::heading;
    body->emplace<MarkdownView>(ui, box->name, box->automation_id() + "-content")->set_markdown(jstr(c, "content"));
    citations_of(ui, *body, c, resolver, box->automation_id());
}

void timeline(Ui& ui, Node& parent, const json& c, const GeneratedCitationResolver& resolver) {
    auto* box = frame_of(ui, parent, c);
    citations_of(ui, *box, c, resolver, box->automation_id());
    auto* list = list_of(*box, box->automation_id(), box->name);
    const auto& items = c["items"];
    for (size_t i = 0; i < items.size(); ++i) {
        const auto& item = items[i];
        auto* row = row_of(*list, box->automation_id(), i);
        const auto time = field(item, "time"), title = field(item, "title"), detail = field(item, "detail"), tag = field(item, "tag");
        row->name = time + u" " + title + (tag.empty() ? u"" : u", " + tag) + (detail.empty() ? u"" : u". " + detail);
        auto* t = row->emplace<Label>(ui, time, TextStyle::body, Paint::text_secondary, true);
        t->width = Sizing::fixed;
        t->fixed_w = 84;
        auto* text = row->emplace<Box>();
        text->gap = 1;
        text->emplace<Label>(ui, title, TextStyle::body, Paint::text, true);
        if (!detail.empty()) text->emplace<Label>(ui, detail, TextStyle::caption, Paint::text_secondary, true);
        citations_of(ui, *text, item, resolver, row->automation_id());
        if (!tag.empty()) row->emplace<Badge>(ui, tag, Paint::surface_alt, Paint::text_secondary);
    }
}

void tasks(Ui& ui, Node& parent, const json& c, const GeneratedCitationResolver& resolver) {
    auto* box = frame_of(ui, parent, c);
    citations_of(ui, *box, c, resolver, box->automation_id());
    auto* list = list_of(*box, box->automation_id(), box->name);
    const auto& items = c["items"];
    for (size_t i = 0; i < items.size(); ++i) {
        const auto& item = items[i];
        auto* row = row_of(*list, box->automation_id(), i);
        const auto state = jstr(item, "state", "open");
        const auto title = field(item, "title"), owner = field(item, "owner"), due = field(item, "due");
        row->name = title + u", " + ui.strings.get("generated.task." + state);
        if (!owner.empty()) row->name += u", " + ui.strings.get("generated.task.owner", {{"owner", owner}});
        if (!due.empty()) row->name += u", " + ui.strings.get("generated.task.due", {{"due", due}});
        const bool done = state == "done", overdue = state == "overdue";
        glyph(*row, ui, done ? kCompleted : overdue ? icon::warning : kRing,
              done ? Paint::success : overdue ? Paint::warning : Paint::text_secondary);
        auto* text = row->emplace<Box>();
        text->gap = 1;
        text->emplace<Label>(ui, title, TextStyle::body, done ? Paint::text_secondary : Paint::text, true);
        if (!owner.empty()) text->emplace<Label>(ui, owner, TextStyle::caption, Paint::text_secondary, true);
        citations_of(ui, *text, item, resolver, row->automation_id());
        if (!due.empty())
            row->emplace<Badge>(ui, due, overdue ? Paint::warning_background : Paint::surface_alt,
                                overdue ? Paint::warning : Paint::text_secondary);
        if (const auto link = jstr(item, "link"); !link.empty()) {
            auto* open = row->emplace<Button>(ui, row->automation_id() + "-open", ui.strings.get("generated.task.open_link"),
                                              Button::Kind::subtle, icon::chevron_right);
            open->role = Role::link;
            open->name = ui.strings.get("generated.task.open_link_name", {{"title", title}});
            open->help = to_utf16(link);
            open->on_click = [&ui, link] {
                if (ui.open_route) ui.open_route(link);
            };
        }
    }
}

std::u16string joined(const json& names) {
    std::u16string out;
    if (!names.is_array()) return out;
    for (const auto& n : names) {
        if (!n.is_string()) continue;
        if (!out.empty()) out += u", ";
        out += to_utf16(n.get_ref<const std::string&>());
    }
    return out;
}

void draft(Ui& ui, Node& parent, const json& c, const GeneratedCitationResolver& resolver) {
    auto* box = parent.emplace<Box>(jstr(c, "id"), Paint::surface_alt, 6, Paint::border);
    box->role = Role::group;
    box->name = field(c, "label");
    box->padding = Insets::all(10);
    box->gap = 6;
    auto* top = box->emplace<Box>();
    top->axis = Axis::row;
    top->gap = 8;
    auto* heading = top->emplace<Label>(ui, box->name, TextStyle::body_strong, Paint::text, true, box->automation_id() + "-label");
    heading->role = Role::heading;
    const auto body = field(c, "body");
    auto* copy = top->emplace<Button>(ui, box->automation_id() + "-copy", ui.strings.get("generated.draft.copy"),
                                      Button::Kind::standard, icon::copy);
    copy->name = ui.strings.get("generated.draft.copy_name", {{"label", box->name}});
    copy->on_click = [&ui, body] {
        if (ui.copy_text) ui.copy_text(body);
        if (ui.announce) ui.announce(ui.strings.get("generated.draft.copied"));
    };
    std::u16string meta;
    const auto part = [&meta](std::u16string text) {
        if (text.empty()) return;
        if (!meta.empty()) meta += u" · ";
        meta += text;
    };
    if (const auto to = joined(c.value("to", json::array())); !to.empty()) part(ui.strings.get("generated.draft.to", {{"names", to}}));
    if (const auto cc = joined(c.value("cc", json::array())); !cc.empty()) part(ui.strings.get("generated.draft.cc", {{"names", cc}}));
    if (const auto subject = field(c, "subject"); !subject.empty()) part(ui.strings.get("generated.draft.subject", {{"subject", subject}}));
    if (!meta.empty()) box->emplace<Label>(ui, meta, TextStyle::caption, Paint::text_secondary, true, box->automation_id() + "-meta");
    box->emplace<MarkdownView>(ui, box->name, box->automation_id() + "-body")->set_plain(body);
    citations_of(ui, *box, c, resolver, box->automation_id());
}

}  // namespace

bool is_generated_display_component(const std::string& type) {
    return type == "markdown" || type == "facts" || type == "callout" || type == "timeline" || type == "tasks" || type == "draft";
}

void add_generated_display_component(Ui& ui, Node& parent, const json& component, const GeneratedCitationResolver& resolver) {
    const auto type = jstr(component, "type");
    if (type == "markdown") markdown(ui, parent, component, resolver);
    else if (type == "facts") facts(ui, parent, component, resolver);
    else if (type == "callout") callout(ui, parent, component, resolver);
    else if (type == "timeline") timeline(ui, parent, component, resolver);
    else if (type == "tasks") tasks(ui, parent, component, resolver);
    else if (type == "draft") draft(ui, parent, component, resolver);
}

void add_generated_template_header(Ui& ui, Node& parent, const json& surface) {
    const std::string kind = jstr(surface, "template");
    const std::u16string title = field(surface, "title");
    if (kind.empty() && title.empty()) return;
    auto* head = parent.emplace<Box>(jstr(surface, "id") + "-header");
    head->gap = 2;
    head->padding.bottom = 4;
    if (!kind.empty()) {
        char16_t g = kDocument;
        if (kind == "daily_report") g = icon::calendar_day;
        else if (kind == "meeting_summary") g = icon::note;
        else if (kind == "action_prep") g = kFlag;
        else if (kind == "meeting_prep") g = kPeople;
        auto* row = head->emplace<Box>();
        row->axis = Axis::row;
        row->gap = 6;
        glyph(*row, ui, g, Paint::accent);
        row->emplace<Label>(ui, ui.strings.get("generated.template." + kind), TextStyle::caption, Paint::text_secondary, false,
                            head->automation_id() + "-template");
    }
    if (!title.empty()) {
        auto* t = head->emplace<Label>(ui, title, TextStyle::heading2, Paint::text, true, head->automation_id() + "-title");
        t->role = Role::heading;
    }
}

}  // namespace wb::ui
