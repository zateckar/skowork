// Classification of computer-use actions (client/computer-use Action Risk Classes, Application Scoping, No Password
// Entry; add-computer-use design D5). Pure functions over facts read from UI Automation, so they are portable and
// unit-tested without a desktop.
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wb::computer {

// What the client reads about an element before acting on it.
struct ElementInfo {
    std::string control_type;   // lower-case UIA control type without the suffix: button, edit, menuitem ...
    std::string name;           // UIA Name
    std::string automation_id;  // UIA AutomationId
    std::string class_name;     // UIA ClassName
    bool is_password = false;   // UIA IsPassword
};

// The action a call performs, as far as classification needs it.
struct ActionInfo {
    std::string tool;     // computer.click, computer.type, computer.keys ...
    std::string keys;     // computer.keys: the combination, for example "Ctrl+Enter"
    bool submit = false;  // computer.type: Enter afterwards
};

// The element's name or automation id is a send, submit, delete, remove, pay or close-without-saving control in
// Czech, German or English (design D5).
bool is_destructive_control(const ElementInfo& e);

// Enter or Ctrl+Enter in a compose window of a known mail or chat client (design D5): `process` is the executable name.
bool sends_message(const ActionInfo& a, std::string_view process);

// The Risk class of an action: "read" for observe and screenshot, "destructive" when the target is a destructive
// control or the keys send a message, else "external". `target` is null when the client could not read the element.
std::string action_risk(const ActionInfo& a, const ElementInfo* target, std::string_view process);

// Typing into this element would enter a credential: a password field, or an element of a credential prompt.
bool is_credential_element(const ElementInfo& e, std::string_view process);

// Executables of credential prompts (Windows credential UI, UAC consent, logon and lock screen).
bool is_credential_process(std::string_view process);

// The application scoping decision (client/computer-use Application Scoping). `process` is the executable file name
// (case-insensitive); `own` names the client's own executables. Returns the reason for a refusal, or nullopt.
std::optional<std::string> app_refusal(std::string_view process, const std::vector<std::string>& allowlist,
                                       const std::vector<std::string>& denylist, const std::vector<std::string>& own);

// The executable a computer.launch program names: a bare name gets .exe. Empty for anything that is not an executable (a
// document, a URL, a command line).
std::string launch_executable(std::string_view program);
// Command interpreters and script hosts, which computer.launch never starts.
bool is_command_interpreter(std::string_view exe);

// A hotkey: modifier flags as RegisterHotKey takes them (MOD_ALT 1, MOD_CONTROL 2, MOD_SHIFT 4, MOD_WIN 8) and a
// virtual-key code.
struct Hotkey {
    unsigned modifiers = 0;
    unsigned vk = 0;
    std::string text;  // as shown to the user: Ctrl+Alt+End
};
// Parses "Ctrl+Alt+End". A combination needs at least two modifiers and one key (client/computer-use Global Stop
// Hotkey); nullopt otherwise.
std::optional<Hotkey> parse_hotkey(std::string_view text);

// One key combination of computer.keys: modifiers (as for Hotkey) and the virtual-key code of the main key.
struct KeyCombo {
    unsigned modifiers = 0;
    unsigned vk = 0;
};
// Parses "Ctrl+S", "Enter", "Alt+F4", "Tab". A space separates combinations pressed one after the other
// ("Down Down Enter"). nullopt when a name is unknown.
std::optional<std::vector<KeyCombo>> parse_keys(std::string_view text);

// Maps a point in a downscaled screenshot back to the screen (client/computer-use Screenshot Perception): the capture
// covered `left, top, width, height` screen pixels and was sent as an image of `image_width x image_height`.
struct CaptureTransform {
    int left = 0, top = 0, width = 0, height = 0;
    int image_width = 0, image_height = 0;
    // nullopt when the point lies outside the image.
    [[nodiscard]] std::optional<std::pair<int, int>> to_screen(double x, double y) const;
};

}  // namespace wb::computer
