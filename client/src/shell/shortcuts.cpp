#include "shell/shortcuts.h"

namespace wb::shell {

using namespace wb::ui;

std::optional<Command> command_for(const KeyEvent& e, const KeyContext& ctx) {
    if (e.ctrl && !e.alt) {
        switch (e.key) {
            case kKeyN: return Command::new_thread;
            case kKeyF: return Command::search;
            case kKeyB: return Command::toggle_rail;
            case kKeyComma: return Command::settings;
            case kKeySlash: return Command::help;
            default: return std::nullopt;
        }
    }
    if (e.alt || e.ctrl) return std::nullopt;
    if (e.key == kKeyEscape) {
        if (ctx.typing) return std::nullopt;
        return ctx.thread_open || ctx.on_list_entry ? std::optional(Command::back) : std::nullopt;
    }
    if (ctx.typing) return std::nullopt;
    // Single keys act on the Thread in focus in the list, or on the open Thread.
    const bool target = ctx.on_list_entry || ctx.thread_open;
    switch (e.key) {
        case kKeyE: return target && !e.shift ? std::optional(Command::archive) : std::nullopt;
        case kKeyU: return target && !e.shift ? std::optional(Command::mark_unread) : std::nullopt;
        case kKeyDelete: return target ? std::optional(Command::remove) : std::nullopt;
        case kKeyDown: return ctx.on_list_entry ? std::optional(Command::next) : std::nullopt;
        case kKeyUp: return ctx.on_list_entry ? std::optional(Command::previous) : std::nullopt;
        case kKeyEnter: return ctx.on_list_entry ? std::optional(Command::open) : std::nullopt;
        default: return std::nullopt;
    }
}

const std::vector<CatalogueEntry>& catalogue() {
    static const std::vector<CatalogueEntry> entries = {
        {"Ctrl+N", "shortcuts.commands.new_thread"}, {"Ctrl+F", "shortcuts.commands.search"},
        {"Ctrl+B", "shortcuts.commands.toggle_rail"}, {"Ctrl+,", "shortcuts.commands.settings"},
        {"Ctrl+/", "shortcuts.commands.help"},       {"E", "shortcuts.commands.archive"},
        {"U", "shortcuts.commands.mark_unread"},     {"Delete", "shortcuts.commands.delete"},
        {"\xE2\x86\x91 / \xE2\x86\x93", "shortcuts.commands.next"}, {"Enter", "shortcuts.commands.open"},
        {"Esc", "shortcuts.commands.back"},
    };
    return entries;
}

}  // namespace wb::shell
