#include "tools/shell_classify.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <memory>
#include <mutex>
#include <regex>

namespace wb::tools {

namespace {

// Remove-Item and its aliases; with -Recurse (any unambiguous prefix, so -r) or cmd's /s.
constexpr const char* kRemove = R"((?:remove-item|\brm\b|\bri\b|\bdel\b|\berase\b|\brd\b|\brmdir\b))";
// The rest of one pipeline element: up to a statement or pipe separator.
constexpr const char* kRest = R"([^;|&\n]*)";

std::vector<std::string> build_defaults() {
    const std::string remove = kRemove, rest = kRest;
    return {
        // Recursive deletion
        remove + rest + R"(\s-r[a-z]*\b)",
        remove + rest + R"(\s/s\b)",
        R"(\[(?:system\.)?io\.directory\]::delete\s*\([^)]*,\s*\$true)",
        // Disk formatting and partitioning
        R"(\b(?:format-volume|clear-disk|initialize-disk|remove-partition|set-partition|diskpart)\b)",
        R"(\bformat(?:\.com)?\s+[a-z]:)",
        R"(\bcipher(?:\.exe)?\s+/w)",
        // Registry deletion
        R"(\breg(?:\.exe)?\s+delete\b)",
        R"(\b(?:remove-itemproperty|clear-itemproperty)\b)",
        remove + rest + R"((?:\bhk(?:lm|cu|cr|u|cc)\b|\bregistry::))",
        // User accounts and security settings
        R"(\bnet(?:\.exe)?\s+(?:user|localgroup|accounts)\b)",
        R"(\b(?:new|set|remove|disable|enable|rename)-localuser\b)",
        R"(\b(?:add|remove)-localgroupmember\b)",
        R"(\bset-executionpolicy\b)",
        R"(\b(?:set|add|remove)-mppreference\b)",
        R"(\bnetsh(?:\.exe)?\b[^;|&\n]*\b(?:advfirewall|firewall)\b)",
        R"(\b(?:new|set|remove|disable|enable)-netfirewall(?:rule|profile)\b)",
        R"(\bset-acl\b)",
        R"(\bicacls(?:\.exe)?\b[^;|&\n]*/(?:grant|deny|remove|reset|setowner|inheritance))",
        R"(\btakeown(?:\.exe)?\b)",
        R"(\b(?:bcdedit|secedit|auditpol)(?:\.exe)?\b)",
        R"(\b(?:disable|enable)-(?:bitlocker|computerrestore)\b)",
        // A nested PowerShell with an encoded command cannot be inspected.
        R"(\b(?:powershell|pwsh)(?:\.exe)?\b[^;|&\n]*\s-e(?:c|n|nc|nco|ncod|ncode|ncoded|ncodedc\w*)?\s+[a-z0-9+/]{16,})",
    };
}

// Compiled patterns, cached by text (policy patterns change rarely).
const std::regex* compiled(const std::string& pattern) {
    static std::mutex mx;
    static std::map<std::string, std::unique_ptr<std::regex>> cache;
    const std::lock_guard lock(mx);
    auto it = cache.find(pattern);
    if (it == cache.end()) {
        std::unique_ptr<std::regex> re;
        try {
            re = std::make_unique<std::regex>(pattern, std::regex::ECMAScript | std::regex::icase | std::regex::optimize);
        } catch (const std::regex_error&) {
            re = nullptr;
        }
        if (cache.size() > 512) cache.clear();
        it = cache.emplace(pattern, std::move(re)).first;
    }
    return it->second.get();
}

bool matches(const std::regex& re, std::string_view command) {
    try {
        return std::regex_search(command.begin(), command.end(), re);
    } catch (const std::regex_error&) {
        return true;  // too complex to decide (stack or complexity limit): assume the worst
    }
}

}  // namespace

const std::vector<std::string>& default_destructive_patterns() {
    static const std::vector<std::string> d = build_defaults();
    return d;
}

std::optional<std::string> destructive_match(std::string_view command, const std::vector<std::string>& extra, std::vector<std::string>* invalid) {
    // Very long commands are not matched in full: the head and the tail are what a model writes.
    std::string probe(command.substr(0, 64 * 1024));
    for (const auto& p : default_destructive_patterns())
        if (const auto* re = compiled(p); re && matches(*re, probe)) return p;
    for (const auto& p : extra) {
        const auto* re = compiled(p);
        if (!re) {
            if (invalid) invalid->push_back(p);
            continue;
        }
        if (matches(*re, probe)) return p;
    }
    return std::nullopt;
}

namespace {

std::string lower(std::string_view s) {
    std::string r(s);
    for (auto& c : r) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return r;
}

// The runtime a program word starts, or "".
std::string runtime_of(std::string_view word) {
    std::string w = lower(word);
    if (const auto slash = w.find_last_of("\\/"); slash != std::string::npos) w = w.substr(slash + 1);
    const auto ends = [&w](std::string_view ext) { return w.size() > ext.size() && w.compare(w.size() - ext.size(), ext.size(), ext) == 0; };
    if (ends(".exe")) w.resize(w.size() - 4);
    static const std::map<std::string, std::string, std::less<>> programs = {
        {"powershell", "powershell"}, {"pwsh", "powershell"}, {"python", "python"}, {"python3", "python"},
        {"pythonw", "python"},        {"py", "python"},       {"pyw", "python"},    {"node", "node"},
        {"nodejs", "node"},           {"cmd", "cmd"},         {"bash", "bash"},     {"sh", "bash"},
        {"wsl", "bash"},              {"ruby", "ruby"},       {"perl", "perl"},     {"wscript", "wsh"},
        {"cscript", "wsh"}};
    if (const auto it = programs.find(w); it != programs.end()) return it->second;
    if (w.rfind("python3.", 0) == 0) return "python";
    static const std::pair<const char*, const char*> extensions[] = {
        {".ps1", "powershell"}, {".psm1", "powershell"}, {".py", "python"}, {".pyw", "python"}, {".js", "node"},
        {".mjs", "node"},       {".cjs", "node"},        {".bat", "cmd"},   {".cmd", "cmd"},    {".sh", "bash"},
        {".rb", "ruby"},        {".pl", "perl"},         {".vbs", "wsh"},   {".wsf", "wsh"}};
    for (const auto& [ext, runtime] : extensions)
        if (ends(ext)) return runtime;
    return {};
}

}  // namespace

std::vector<std::string> script_runtimes(std::string_view command) {
    // Words and separators, PowerShell-like: quotes group, # starts a comment, ; | && || ( ) { } and new lines end a
    // statement or pipeline element, a lone & is the call operator.
    struct Token {
        std::string text;
        bool sep = false;
    };
    std::vector<Token> tokens;
    const std::string_view c = command.substr(0, 64 * 1024);
    for (size_t i = 0; i < c.size();) {
        const char ch = c[i];
        if (ch == ' ' || ch == '\t') {
            ++i;
        } else if (ch == '#') {
            while (i < c.size() && c[i] != '\n') ++i;
        } else if (std::string_view(";|\r\n(){}").find(ch) != std::string_view::npos) {
            tokens.push_back({"", true});
            ++i;
        } else if (ch == '&') {
            if (i + 1 < c.size() && c[i + 1] == '&') {
                tokens.push_back({"", true});
                i += 2;
            } else {
                tokens.push_back({"&", false});
                ++i;
            }
        } else {
            std::string word;
            while (i < c.size() && std::string_view(" \t;|\r\n(){}&").find(c[i]) == std::string_view::npos) {
                if (c[i] == '"' || c[i] == '\'') {
                    const char q = c[i++];
                    while (i < c.size() && c[i] != q) word += c[i++];
                    if (i < c.size()) ++i;
                } else {
                    word += c[i++];
                }
            }
            tokens.push_back({std::move(word), false});
        }
    }
    std::vector<std::string> found;
    const auto add = [&found](const std::string& r) {
        if (!r.empty() && std::find(found.begin(), found.end(), r) == found.end()) found.push_back(r);
    };
    bool at_command = true, start_process = false;
    for (const auto& t : tokens) {
        if (t.sep) {
            at_command = true;
            start_process = false;
            continue;
        }
        if (start_process) {
            if (t.text.empty() || t.text[0] == '-') continue;  // -FilePath, -NoNewWindow...
            add(runtime_of(t.text));
            start_process = false;
            continue;
        }
        if (!at_command) continue;
        const std::string w = lower(t.text);
        if (w == "&" || w == ".") continue;  // call operators: the program follows
        // An assignment ($out = python x.py): the program follows the variable and the operator.
        if (w.starts_with('$') || (!w.empty() && w.size() <= 2 && w.back() == '=')) continue;
        if (w == "start-process" || w == "saps" || w == "start") {
            start_process = true;
            at_command = false;
            continue;
        }
        add(runtime_of(t.text));
        at_command = false;
    }
    return found;
}

}  // namespace wb::tools
