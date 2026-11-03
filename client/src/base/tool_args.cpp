#include "base/tool_args.h"

#include "base/strings.h"

#include <algorithm>
#include <cctype>

namespace wb {
namespace {

bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
bool is_ident_start(char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_' || c == '$'; }
bool is_ident_char(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '$'; }
bool is_number_char(char c) { return std::isdigit(static_cast<unsigned char>(c)) || c == '+' || c == '-' || c == '.' || c == 'e' || c == 'E'; }

// Deepest bracket nesting outside strings (either quote style); the text may be malformed.
int nesting_depth(std::string_view t) {
    int depth = 0, deepest = 0;
    char quote = 0;
    for (size_t i = 0; i < t.size(); ++i) {
        const char c = t[i];
        if (quote) {
            if (c == '\\') ++i;
            else if (c == quote) quote = 0;
            continue;
        }
        if (c == '"' || c == '\'') quote = c;
        else if (c == '{' || c == '[') deepest = std::max(deepest, ++depth);
        else if ((c == '}' || c == ']') && depth > 0) --depth;
    }
    return deepest;
}

std::string_view strip_fence(std::string_view t) {
    while (!t.empty() && is_space(t.front())) t.remove_prefix(1);
    while (!t.empty() && is_space(t.back())) t.remove_suffix(1);
    if (t.size() < 6 || t.substr(0, 3) != "```" || t.substr(t.size() - 3) != "```") return t;
    t.remove_prefix(3);
    t.remove_suffix(3);
    // The rest of the opening line is the language tag ("json").
    const auto nl = t.find('\n');
    if (nl != std::string_view::npos) {
        const std::string_view tag = t.substr(0, nl);
        bool tag_ok = true;
        for (const char c : tag)
            if (!std::isalnum(static_cast<unsigned char>(c)) && c != '\r' && c != ' ') tag_ok = false;
        if (tag_ok) t.remove_prefix(nl + 1);
    } else {
        // ```{...}``` on one line, optionally with a tag directly before the brace.
        const auto brace = t.find_first_of("{[");
        if (brace != std::string_view::npos) t.remove_prefix(brace);
    }
    while (!t.empty() && is_space(t.front())) t.remove_prefix(1);
    while (!t.empty() && is_space(t.back())) t.remove_suffix(1);
    return t;
}

void append_escaped_control(std::string& out, char ch) {
    switch (ch) {
    case '\n': out += "\\n"; break;
    case '\r': out += "\\r"; break;
    case '\t': out += "\\t"; break;
    default: {
        static const char* hex = "0123456789abcdef";
        const auto u = static_cast<unsigned char>(ch);
        out += "\\u00";
        out += hex[u >> 4];
        out += hex[u & 15];
    }
    }
}

ParsedArguments fail(std::string message) {
    ParsedArguments r;
    r.error = std::move(message);
    return r;
}

ParsedArguments parse_impl(std::string_view text, bool allow_unwrap) {
    if (text.size() > kMaxToolArgumentBytes) return fail("arguments are too large");
    if (nesting_depth(text) > kMaxToolArgumentDepth) return fail("arguments are nested too deeply");
    const std::string trimmed = trim(text);
    if (trimmed.empty()) {
        ParsedArguments r;
        r.value = json::object();
        return r;
    }

    std::optional<json> parsed = parse_json(trimmed);
    bool repaired = false;
    if (!parsed) {
        if (auto fixed = repair_json_text(trimmed)) {
            parsed = parse_json(*fixed);
            repaired = parsed.has_value();
        }
    }
    if (!parsed) return fail("arguments are not valid JSON");

    if (parsed->is_null()) {
        ParsedArguments r;
        r.value = json::object();
        r.repaired = repaired;
        return r;
    }
    if (parsed->is_string() && allow_unwrap) {
        // Double encoding: the arguments arrived as a JSON string that holds the object.
        ParsedArguments inner = parse_impl(parsed->get_ref<const std::string&>(), false);
        if (inner.value) inner.repaired = true;
        return inner;
    }
    if (!parsed->is_object()) return fail("arguments must be a JSON object");
    ParsedArguments r;
    r.value = std::move(*parsed);
    r.repaired = repaired;
    return r;
}

}  // namespace

std::optional<std::string> repair_json_text(std::string_view input) {
    const std::string_view t = strip_fence(input);
    const size_t n = t.size();
    std::string out;
    out.reserve(n + 16);
    size_t i = 0;
    while (i < n) {
        const char c = t[i];
        if (c == '"' || c == '\'') {
            const char quote = c;
            out += '"';
            ++i;
            bool closed = false;
            while (i < n) {
                const char ch = t[i];
                if (ch == '\\') {
                    if (i + 1 >= n) return std::nullopt;
                    const char nx = t[i + 1];
                    if (nx == '"' || nx == '\\' || nx == '/' || nx == 'b' || nx == 'f' || nx == 'n' || nx == 'r' ||
                        nx == 't' || nx == 'u') {
                        out += '\\';
                        out += nx;
                        i += 2;
                    } else if (nx == '\'') {
                        out += '\'';
                        i += 2;
                    } else {
                        out += "\\\\";  // a lone backslash (a Windows path, a regex): keep it literally
                        ++i;
                    }
                } else if (ch == quote) {
                    out += '"';
                    ++i;
                    closed = true;
                    break;
                } else if (ch == '"') {
                    out += "\\\"";  // a double quote inside a single-quoted string
                    ++i;
                } else if (static_cast<unsigned char>(ch) < 0x20) {
                    append_escaped_control(out, ch);
                    ++i;
                } else {
                    out += ch;
                    ++i;
                }
            }
            if (!closed) return std::nullopt;  // a truncated string is ambiguous
        } else if (c == ',') {
            size_t j = i + 1;
            while (j < n && is_space(t[j])) ++j;
            if (j < n && (t[j] == '}' || t[j] == ']')) {
                ++i;  // trailing comma
            } else {
                out += c;
                ++i;
            }
        } else if (std::isdigit(static_cast<unsigned char>(c)) || c == '-' || c == '+' || c == '.') {
            while (i < n && is_number_char(t[i])) out += t[i++];
        } else if (is_ident_start(c)) {
            size_t j = i;
            while (j < n && is_ident_char(t[j])) ++j;
            const std::string_view word = t.substr(i, j - i);
            size_t k = j;
            while (k < n && is_space(t[k])) ++k;
            const bool is_key = k < n && t[k] == ':';
            if (word == "true" || word == "false" || word == "null") {
                if (is_key) out += "\"" + std::string(word) + "\"";
                else out += word;
            } else if (!is_key && word == "True") {
                out += "true";
            } else if (!is_key && word == "False") {
                out += "false";
            } else if (!is_key && word == "None") {
                out += "null";
            } else if (is_key) {
                out += "\"" + std::string(word) + "\"";  // unquoted key
            } else {
                return std::nullopt;  // a bare word in a value position is ambiguous
            }
            i = j;
        } else {
            out += c;
            ++i;
        }
    }
    if (out == input) return std::nullopt;
    return out;
}

ParsedArguments parse_tool_arguments(std::string_view text) { return parse_impl(text, true); }

}  // namespace wb
