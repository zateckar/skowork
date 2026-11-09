#include "computer/classify.h"

#include "base/strings.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>

namespace wb::computer {

namespace {

// Word stems of destructive controls (design D5), folded: send, submit, delete, remove, pay, close without saving.
constexpr std::string_view kDestructiveStems[] = {
    // en
    "send", "submit", "delete", "remove", "pay", "purchase", "discard", "close without saving", "dont save", "don't save",
    "do not save", "erase",
    // cs: odeslat/odeslání, podat, smazat, odstranit, vymazat, odebrat, zaplatit, platba, uhradit, neukládat, bez uložení,
    // zahodit
    "odesl", "podat", "smaz", "odstran", "vymaz", "odebr", "zaplat", "platb", "uhrad", "neuklad", "bez ulozeni", "zahod",
    // de: senden, absenden, versenden, einreichen, übermitteln, löschen, entfernen, bezahlen, Zahlung, nicht speichern,
    // ohne Speichern, verwerfen
    "senden", "absend", "versend", "einreich", "ubermittel", "losch", "entfern", "bezahl", "zahlung", "nicht speichern",
    "ohne speichern", "verwerf",
};

// Splits an automation id such as "btnSendMail" or "delete_button" into words: "btn send mail".
std::string words_of_id(std::string_view id) {
    std::string out;
    for (std::size_t i = 0; i < id.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(id[i]);
        if (!std::isalnum(c)) {
            out.push_back(' ');
            continue;
        }
        if (std::isupper(c) && i > 0 && std::islower(static_cast<unsigned char>(id[i - 1]))) out.push_back(' ');
        out.push_back(static_cast<char>(std::tolower(c)));
    }
    return out;
}

bool has_stem_at_word_start(const std::string& text) {
    for (const std::string_view stem : kDestructiveStems) {
        for (std::size_t pos = text.find(stem); pos != std::string::npos; pos = text.find(stem, pos + 1)) {
            if (pos == 0 || !std::isalnum(static_cast<unsigned char>(text[pos - 1]))) return true;
        }
    }
    return false;
}

std::string exe_key(std::string_view process) {
    std::string p = to_lower(trim(process));
    if (const auto slash = p.find_last_of("\\/"); slash != std::string::npos) p = p.substr(slash + 1);
    if (!ends_with(p, ".exe")) p += ".exe";
    return p;
}

bool listed(const std::vector<std::string>& list, const std::string& key) {
    return std::any_of(list.begin(), list.end(), [&](const std::string& x) { return !trim(x).empty() && exe_key(x) == key; });
}

// Mail and chat clients whose compose windows send on Enter or Ctrl+Enter.
constexpr std::string_view kMessengers[] = {"outlook.exe", "olk.exe",    "thunderbird.exe", "teams.exe",    "ms-teams.exe",
                                            "slack.exe",   "skype.exe",  "lync.exe",        "whatsapp.exe", "signal.exe",
                                            "telegram.exe"};

constexpr unsigned kModAlt = 1, kModControl = 2, kModShift = 4, kModWin = 8;
constexpr unsigned kVkReturn = 0x0D, kVkDelete = 0x2E;

std::optional<unsigned> modifier_of(const std::string& t) {
    if (t == "ctrl" || t == "control") return kModControl;
    if (t == "alt") return kModAlt;
    if (t == "shift") return kModShift;
    if (t == "win" || t == "windows" || t == "meta" || t == "super") return kModWin;
    return std::nullopt;
}

std::optional<unsigned> vk_of(const std::string& t) {
    struct Named {
        std::string_view name;
        unsigned vk;
    };
    static constexpr Named names[] = {
        {"end", 0x23},       {"home", 0x24},     {"insert", 0x2D},  {"ins", 0x2D},      {"delete", 0x2E},  {"del", 0x2E},
        {"pageup", 0x21},    {"pgup", 0x21},     {"pagedown", 0x22}, {"pgdn", 0x22},    {"pause", 0x13},   {"break", 0x13},
        {"escape", 0x1B},    {"esc", 0x1B},      {"enter", 0x0D},   {"return", 0x0D},   {"tab", 0x09},     {"space", 0x20},
        {"backspace", 0x08}, {"left", 0x25},     {"up", 0x26},      {"right", 0x27},    {"down", 0x28},    {"apps", 0x5D},
        {"menu", 0x5D},      {"printscreen", 0x2C}, {"capslock", 0x14},
    };
    for (const auto& n : names)
        if (t == n.name) return n.vk;
    if (t.size() == 1 && t[0] >= 'a' && t[0] <= 'z') return 0x41u + static_cast<unsigned>(t[0] - 'a');
    if (t.size() == 1 && t[0] >= '0' && t[0] <= '9') return 0x30u + static_cast<unsigned>(t[0] - '0');
    if (t.size() >= 2 && t.size() <= 3 && t[0] == 'f') {
        const int n = std::atoi(t.c_str() + 1);
        if (n >= 1 && n <= 24 && std::to_string(n) == t.substr(1)) return 0x70u + static_cast<unsigned>(n - 1);
    }
    return std::nullopt;
}

// One "Ctrl+Shift+S" combination; a lone modifier is the key itself.
std::optional<KeyCombo> parse_combo(std::string_view text) {
    KeyCombo c;
    bool have_key = false;
    std::vector<std::string> parts = split(text, '+');
    for (std::size_t i = 0; i < parts.size(); ++i) {
        std::string t = to_lower(trim(parts[i]));
        t.erase(std::remove(t.begin(), t.end(), ' '), t.end());
        if (t.empty()) return std::nullopt;
        const auto mod = modifier_of(t);
        if (mod && i + 1 < parts.size()) {
            c.modifiers |= *mod;
            continue;
        }
        if (have_key) return std::nullopt;
        if (mod) {
            c.vk = *mod == kModControl ? 0x11 : *mod == kModAlt ? 0x12 : *mod == kModShift ? 0x10 : 0x5B;
        } else {
            const auto vk = vk_of(t);
            if (!vk) return std::nullopt;
            c.vk = *vk;
        }
        have_key = true;
    }
    if (!have_key) return std::nullopt;
    return c;
}

}  // namespace

bool is_destructive_control(const ElementInfo& e) {
    return has_stem_at_word_start(fold_latin(e.name)) || has_stem_at_word_start(words_of_id(e.automation_id));
}

bool sends_message(const ActionInfo& a, std::string_view process) {
    const std::string key = exe_key(process);
    if (std::find(std::begin(kMessengers), std::end(kMessengers), key) == std::end(kMessengers)) return false;
    if (a.tool == "computer.type") return a.submit;
    if (a.tool != "computer.keys") return false;
    const auto combos = parse_keys(a.keys);
    if (!combos) return false;
    return std::any_of(combos->begin(), combos->end(), [](const KeyCombo& c) { return c.vk == kVkReturn; });
}

std::string action_risk(const ActionInfo& a, const ElementInfo* target, std::string_view process) {
    if (a.tool == "computer.observe" || a.tool == "computer.screenshot") return "read";
    if (sends_message(a, process)) return "destructive";
    if (a.tool == "computer.keys") {
        // Delete outside a text field removes the selected item (a file, a mail, a row); Shift+Delete removes it for good.
        if (const auto combos = parse_keys(a.keys)) {
            const bool in_text = target && (target->control_type == "edit" || target->control_type == "document");
            for (const auto& c : *combos)
                if (c.vk == kVkDelete && ((c.modifiers & kModShift) || !in_text)) return "destructive";
        }
    }
    // Typing only fills the field; with submit (Enter) it also activates the target, as the other actions do.
    const bool activates = a.tool != "computer.type" || a.submit;
    if (target && activates && is_destructive_control(*target)) return "destructive";
    return "external";
}

bool is_credential_process(std::string_view process) {
    static constexpr std::string_view names[] = {"credentialuibroker.exe", "consent.exe", "logonui.exe", "lockapp.exe", "credwiz.exe"};
    const std::string key = exe_key(process);
    return std::find(std::begin(names), std::end(names), key) != std::end(names);
}

bool is_credential_element(const ElementInfo& e, std::string_view process) {
    return e.is_password || is_credential_process(process) || e.class_name == "Credential Dialog Xaml Host";
}

std::optional<std::string> app_refusal(std::string_view process, const std::vector<std::string>& allowlist,
                                       const std::vector<std::string>& denylist, const std::vector<std::string>& own) {
    const std::string key = exe_key(process);
    if (trim(process).empty()) return "the application of the window is unknown";
    if (listed(own, key)) return "SkoWork's own windows are always out of scope";
    if (is_credential_process(key)) return key + " is a credential or sign-in prompt, which is always out of scope";
    static constexpr std::string_view security[] = {"sechealthui.exe", "securityhealthhost.exe", "securityhealthsystray.exe", "msascui.exe"};
    if (std::find(std::begin(security), std::end(security), key) != std::end(security))
        return key + " is a security tool, which is always out of scope";
    if (listed(denylist, key)) return key + " is on the application denylist (policy.tools.computer.app_denylist)";
    if (std::any_of(allowlist.begin(), allowlist.end(), [](const std::string& x) { return !trim(x).empty(); }) && !listed(allowlist, key))
        return key + " is not on the application allowlist (policy.tools.computer.app_allowlist)";
    return std::nullopt;
}

std::string launch_executable(std::string_view program) {
    const std::string p = trim(program);
    if (p.empty() || p.find('"') != std::string::npos) return {};
    const size_t colon = p.find(':');
    if (colon != std::string::npos && colon != 1) return {};
    const bool absolute = colon == 1 || p.compare(0, 2, "\\\\") == 0;
    if (!absolute && p.find_first_of(" \t") != std::string::npos) return {};
    // Whitespace inside a directory name of an absolute path is fine; before a separator it starts an argument.
    for (size_t i = 1; i < p.size(); ++i)
        if ((p[i] == '\\' || p[i] == '/') && (p[i - 1] == ' ' || p[i - 1] == '\t')) return {};
    const size_t sep = p.find_last_of("\\/");
    const std::string name = sep == std::string::npos ? p : p.substr(sep + 1);
    if (name.empty() || name.find_first_of(" \t") != std::string::npos) return {};
    const size_t dot = name.find_last_of('.');
    if (dot == std::string::npos) return name + ".exe";
    return iequals(name.substr(dot), ".exe") ? name : std::string{};
}

bool is_command_interpreter(std::string_view exe) {
    static constexpr std::string_view names[] = {"cmd.exe",   "powershell.exe", "pwsh.exe",  "wscript.exe",
                                                 "cscript.exe", "mshta.exe",    "rundll32.exe"};
    return std::any_of(std::begin(names), std::end(names), [&](std::string_view n) { return iequals(exe, n); });
}

std::optional<Hotkey> parse_hotkey(std::string_view text) {
    const auto c = parse_combo(text);
    if (!c) return std::nullopt;
    int mods = 0;
    for (const unsigned m : {kModControl, kModAlt, kModShift, kModWin}) mods += (c->modifiers & m) ? 1 : 0;
    if (mods < 2) return std::nullopt;
    Hotkey h{c->modifiers, c->vk, {}};
    // The display form: modifiers in a fixed order, then the key as written.
    if (c->modifiers & kModControl) h.text += "Ctrl+";
    if (c->modifiers & kModAlt) h.text += "Alt+";
    if (c->modifiers & kModShift) h.text += "Shift+";
    if (c->modifiers & kModWin) h.text += "Win+";
    std::string key = trim(split(text, '+').back());
    if (!key.empty()) key[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(key[0])));
    h.text += key;
    return h;
}

std::optional<std::vector<KeyCombo>> parse_keys(std::string_view text) {
    std::vector<KeyCombo> out;
    std::string token;
    const auto flush = [&]() -> bool {
        if (token.empty()) return true;
        const auto c = parse_combo(token);
        token.clear();
        if (!c) return false;
        out.push_back(*c);
        return true;
    };
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char ch = text[i];
        // "Ctrl + S" is one combination: spaces next to a plus do not separate.
        const bool next_plus = [&] {
            std::size_t j = i;
            while (j < text.size() && text[j] == ' ') ++j;
            return j < text.size() && text[j] == '+';
        }();
        if (ch == ' ' && !next_plus && !token.empty() && token.back() != '+') {
            if (!flush()) return std::nullopt;
        } else if (ch != ' ') {
            token.push_back(ch);
        }
    }
    if (!flush() || out.empty()) return std::nullopt;
    return out;
}

std::optional<std::pair<int, int>> CaptureTransform::to_screen(double x, double y) const {
    if (image_width <= 0 || image_height <= 0 || !std::isfinite(x) || !std::isfinite(y)) return std::nullopt;
    if (x < 0 || y < 0 || x >= image_width || y >= image_height) return std::nullopt;
    // The centre of the screen area the image pixel stands for.
    const int sx = left + static_cast<int>(std::floor((x + 0.5) * width / image_width));
    const int sy = top + static_cast<int>(std::floor((y + 0.5) * height / image_height));
    return std::make_pair(std::min(sx, left + width - 1), std::min(sy, top + height - 1));
}

}  // namespace wb::computer
