// The keyboard shortcut catalogue (client/work-inbox Keyboard Shortcuts) as a mapping from key presses to commands,
// ported from the web UI's shortcuts.ts. Keys are Windows virtual-key codes.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "ui/layout.h"

namespace wb::shell {

enum class Command { new_thread, search, toggle_rail, settings, help, archive, mark_unread, remove, next, previous, open, back };

struct KeyContext {
    bool typing = false;         // focus is in a text field, where single keys type text
    bool on_list_entry = false;  // focus is on a Thread in the list
    bool thread_open = false;    // a Thread is open
};

// The command for a key press, or nullopt when the key is not a shortcut in this context.
std::optional<Command> command_for(const ui::KeyEvent& e, const KeyContext& ctx);

// The catalogue in display order for the help dialog: the keys and the string key of the command.
struct CatalogueEntry {
    const char* keys;
    const char* label_key;
};
const std::vector<CatalogueEntry>& catalogue();

}  // namespace wb::shell
