#include "skills/yaml.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace wb::skills {
namespace {

struct Line {
    int no = 0;       // 1-based line number
    int indent = 0;   // leading spaces
    std::string text; // without indentation and trailing comment/space
    std::string raw;  // the whole line (block scalars)
};

struct Fail {
    std::string message;
};

[[noreturn]] void fail(int line, const std::string& what) { throw Fail{"line " + std::to_string(line) + ": " + what}; }

// Removes a trailing comment (" #" outside quotes) and trailing spaces.
std::string strip_comment(const std::string& s) {
    char quote = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (quote) {
            if (c == '\\' && quote == '"') ++i;
            else if (c == quote) quote = 0;
        } else if (c == '"' || c == '\'') {
            if (i == 0 || s[i - 1] == ' ' || s[i - 1] == '[' || s[i - 1] == '{' || s[i - 1] == ',' || s[i - 1] == ':') quote = c;
        } else if (c == '#' && (i == 0 || s[i - 1] == ' ' || s[i - 1] == '\t')) {
            const size_t end = i ? s.find_last_not_of(" \t", i - 1) : std::string::npos;
            return end == std::string::npos ? std::string() : s.substr(0, end + 1);
        }
    }
    const size_t end = s.find_last_not_of(" \t");
    return end == std::string::npos ? std::string() : s.substr(0, end + 1);
}

std::string trim(std::string_view s) {
    const size_t b = s.find_first_not_of(" \t");
    if (b == std::string_view::npos) return {};
    const size_t e = s.find_last_not_of(" \t");
    return std::string(s.substr(b, e - b + 1));
}

class Parser {
public:
    Parser(std::vector<Line> lines, std::vector<std::string> raw, const YamlLimits& limits)
        : lines_(std::move(lines)), raw_(std::move(raw)), limits_(limits) {}

    json document() {
        if (lines_.empty()) return json::object();
        if (lines_[0].indent != 0) fail(lines_[0].no, "the document must start at column 1");
        json root = block(0, 0);
        if (pos_ < lines_.size()) fail(lines_[pos_].no, "unexpected indentation");
        if (!root.is_object()) fail(lines_[0].no, "the frontmatter must be a mapping of fields");
        return root;
    }

private:
    // A block whose lines start at exactly `indent`: a mapping or a sequence.
    json block(int indent, int depth) {
        if (depth > limits_.max_depth) fail(lines_[pos_].no, "nesting is too deep");
        const Line& first = lines_[pos_];
        if (first.text == "-" || first.text.rfind("- ", 0) == 0) return sequence(indent, depth);
        return mapping(indent, depth);
    }

    json mapping(int indent, int depth) {
        json out = json::object();
        while (pos_ < lines_.size() && lines_[pos_].indent == indent) {
            const Line& l = lines_[pos_];
            if (l.text == "-" || l.text.rfind("- ", 0) == 0) fail(l.no, "a list item where a field was expected");
            const auto [key, rest] = split_key(l);
            if (out.contains(key)) fail(l.no, "the field " + key + " appears twice");
            ++pos_;
            out[key] = value_after(rest, indent, depth, l.no);
        }
        return out;
    }

    json sequence(int indent, int depth) {
        json out = json::array();
        while (pos_ < lines_.size() && lines_[pos_].indent == indent && (lines_[pos_].text == "-" || lines_[pos_].text.rfind("- ", 0) == 0)) {
            Line& l = lines_[pos_];
            const std::string item = l.text == "-" ? std::string() : trim(std::string_view(l.text).substr(2));
            if (item.empty()) {
                ++pos_;
                out.push_back(nested_or_null(indent, depth));
                continue;
            }
            // "- key: value" opens a mapping whose further keys are indented to the item's column.
            if (looks_like_key(item)) {
                const int item_indent = indent + static_cast<int>(l.text.size() - item.size());
                l.indent = item_indent;
                l.text = item;
                out.push_back(mapping(item_indent, depth + 1));
                continue;
            }
            ++pos_;
            out.push_back(scalar_or_flow(item, l.no, depth));
        }
        return out;
    }

    json nested_or_null(int indent, int depth) {
        if (pos_ < lines_.size() && lines_[pos_].indent > indent) return block(lines_[pos_].indent, depth + 1);
        return nullptr;
    }

    json value_after(const std::string& rest, int indent, int depth, int no) {
        if (rest.empty()) {
            // A nested block, or a sequence at the same indentation as the key ("key:\n- a").
            if (pos_ < lines_.size() && (lines_[pos_].indent > indent ||
                                         (lines_[pos_].indent == indent && (lines_[pos_].text == "-" || lines_[pos_].text.rfind("- ", 0) == 0)))) {
                if (lines_[pos_].indent == indent) return sequence(indent, depth + 1);
                return block(lines_[pos_].indent, depth + 1);
            }
            return nullptr;
        }
        if (rest[0] == '|' || rest[0] == '>') return block_scalar(rest, indent, no);
        return scalar_or_flow(rest, no, depth);
    }

    // Literal (|) or folded (>) scalar: the more-indented raw lines that follow the header line `no`, including blank
    // lines and lines that start with # (they are text here, not comments).
    json block_scalar(const std::string& header, int indent, int no) {
        if (header.size() > 2 || (header.size() == 2 && header[1] != '-' && header[1] != '+')) fail(no, "unsupported block scalar header " + header);
        const bool literal = header[0] == '|';
        const char chomp = header.size() == 2 ? header[1] : 0;
        std::vector<std::string> parts;
        int body_indent = -1;
        int last = no;
        for (int k = no + 1; k <= static_cast<int>(raw_.size()); ++k) {
            const std::string& r = raw_[static_cast<size_t>(k - 1)];
            const size_t first = r.find_first_not_of(' ');
            if (first == std::string::npos) {
                parts.emplace_back();
                continue;
            }
            if (static_cast<int>(first) <= indent) break;
            if (body_indent < 0) body_indent = static_cast<int>(first);
            if (static_cast<int>(first) < body_indent) fail(k, "a block text line is indented less than the first one");
            parts.push_back(r.substr(static_cast<size_t>(body_indent)));
            last = k;
        }
        parts.resize(static_cast<size_t>(std::max(0, last - no)));  // trailing blank lines are not part of the text
        while (pos_ < lines_.size() && lines_[pos_].no <= last) ++pos_;
        std::string out;
        for (size_t i = 0; i < parts.size(); ++i) {
            if (i) out += literal || parts[i].empty() || parts[i - 1].empty() ? "\n" : " ";
            out += parts[i];
        }
        if (chomp != '-' && !out.empty()) out += "\n";
        return out;
    }

    static bool looks_like_key(const std::string& s) {
        if (s.empty() || s[0] == '"' || s[0] == '\'' || s[0] == '[' || s[0] == '{') return false;
        const size_t c = s.find(':');
        return c != std::string::npos && c > 0 && (c + 1 == s.size() || s[c + 1] == ' ');
    }

    static std::pair<std::string, std::string> split_key(const Line& l) {
        if (!looks_like_key(l.text)) fail(l.no, "expected \"field: value\"");
        const size_t c = l.text.find(':');
        std::string key = trim(std::string_view(l.text).substr(0, c));
        for (char ch : key)
            if (!(std::isalnum(static_cast<unsigned char>(ch)) || ch == '_' || ch == '-' || ch == '.' || ch == '*' || ch == ':' || ch == '/'))
                fail(l.no, "the field name \"" + key + "\" is not supported");
        return {key, trim(std::string_view(l.text).substr(c + 1))};
    }

    json scalar_or_flow(const std::string& s, int no, int depth) {
        if (s[0] == '[' || s[0] == '{') {
            size_t i = 0;
            json v = flow(s, i, no, depth);
            skip_spaces(s, i);
            if (i != s.size()) fail(no, "unexpected text after a list or map");
            return v;
        }
        if (s[0] == '&' || s[0] == '*' || s[0] == '!' || s.rfind("---", 0) == 0) fail(no, "anchors, aliases, tags and documents are not supported");
        size_t i = 0;
        json v = scalar(s, i, no, false);
        skip_spaces(s, i);
        if (i != s.size()) fail(no, "unexpected text after a value");
        return v;
    }

    static void skip_spaces(const std::string& s, size_t& i) {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    }

    json flow(const std::string& s, size_t& i, int no, int depth) {
        if (depth > limits_.max_depth) fail(no, "nesting is too deep");
        const char open = s[i++];
        const char close = open == '[' ? ']' : '}';
        json out = open == '[' ? json::array() : json::object();
        skip_spaces(s, i);
        if (i < s.size() && s[i] == close) return ++i, out;
        for (;;) {
            skip_spaces(s, i);
            if (i >= s.size()) fail(no, "an unclosed list or map");
            if (open == '[') {
                out.push_back(s[i] == '[' || s[i] == '{' ? flow(s, i, no, depth + 1) : scalar(s, i, no, true));
            } else {
                json k = scalar(s, i, no, true, true);
                skip_spaces(s, i);
                if (i >= s.size() || s[i] != ':') fail(no, "expected \":\" in a map");
                ++i;
                skip_spaces(s, i);
                if (!k.is_string()) fail(no, "map keys must be text");
                out[k.get<std::string>()] = i < s.size() && (s[i] == '[' || s[i] == '{') ? flow(s, i, no, depth + 1) : scalar(s, i, no, true);
            }
            skip_spaces(s, i);
            if (i < s.size() && s[i] == ',') {
                ++i;
                continue;
            }
            if (i < s.size() && s[i] == close) return ++i, out;
            fail(no, "expected \",\" or the end of the list or map");
        }
    }

    // A scalar at s[i]. In flow context plain scalars end at , ] } (and : for keys).
    static json scalar(const std::string& s, size_t& i, int no, bool in_flow, bool key = false) {
        if (i < s.size() && s[i] == '"') {
            std::string out;
            for (++i; i < s.size(); ++i) {
                char c = s[i];
                if (c == '"') return ++i, json(out);
                if (c == '\\' && i + 1 < s.size()) {
                    c = s[++i];
                    switch (c) {
                    case 'n': out += '\n'; break;
                    case 't': out += '\t'; break;
                    case 'r': out += '\r'; break;
                    case '0': out += '\0'; break;
                    case '"': case '\\': case '/': out += c; break;
                    case 'u': {
                        if (i + 4 >= s.size()) fail(no, "a bad \\u escape");
                        unsigned cp = 0;
                        for (int k = 0; k < 4; ++k) {
                            const char h = s[++i];
                            cp = cp * 16 + static_cast<unsigned>(std::isdigit(static_cast<unsigned char>(h)) ? h - '0' : (std::tolower(static_cast<unsigned char>(h)) - 'a' + 10));
                            if (!std::isxdigit(static_cast<unsigned char>(h))) fail(no, "a bad \\u escape");
                        }
                        if (cp < 0x80) out += static_cast<char>(cp);
                        else if (cp < 0x800) out += static_cast<char>(0xC0 | (cp >> 6)), out += static_cast<char>(0x80 | (cp & 0x3F));
                        else out += static_cast<char>(0xE0 | (cp >> 12)), out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)), out += static_cast<char>(0x80 | (cp & 0x3F));
                        break;
                    }
                    default: fail(no, "an unknown escape \\" + std::string(1, c));
                    }
                } else {
                    out += c;
                }
            }
            fail(no, "an unclosed quoted value");
        }
        if (i < s.size() && s[i] == '\'') {
            std::string out;
            for (++i; i < s.size(); ++i) {
                if (s[i] == '\'') {
                    if (i + 1 < s.size() && s[i + 1] == '\'') {
                        out += '\'';
                        ++i;
                        continue;
                    }
                    return ++i, json(out);
                }
                out += s[i];
            }
            fail(no, "an unclosed quoted value");
        }
        const size_t start = i;
        while (i < s.size()) {
            const char c = s[i];
            if (in_flow && (c == ',' || c == ']' || c == '}')) break;
            if (key && c == ':') break;
            ++i;
        }
        const std::string text = trim(std::string_view(s).substr(start, i - start));
        if (text == "true" || text == "True" || text == "TRUE") return true;
        if (text == "false" || text == "False" || text == "FALSE") return false;
        if (text == "null" || text == "~" || text == "Null" || text == "NULL") return nullptr;
        if (text.empty() && !in_flow) return nullptr;
        return text;
    }

    std::vector<Line> lines_;
    std::vector<std::string> raw_;  // every line of the document, by line number - 1
    YamlLimits limits_;
    size_t pos_ = 0;
};

}  // namespace

Result<json> parse_yaml(std::string_view text, const YamlLimits& limits) {
    if (text.size() > limits.max_bytes) return make_error("too_large", "the frontmatter is larger than " + std::to_string(limits.max_bytes) + " bytes");
    std::vector<Line> lines;
    std::vector<std::string> all;
    int no = 0;
    size_t p = 0;
    while (p <= text.size()) {
        size_t e = text.find('\n', p);
        if (e == std::string_view::npos) e = text.size();
        std::string raw(text.substr(p, e - p));
        if (!raw.empty() && raw.back() == '\r') raw.pop_back();
        ++no;
        p = e + 1;
        all.push_back(raw);
        if (raw.find('\t') != std::string::npos && raw.find_first_not_of(" \t") != std::string::npos &&
            raw.find_first_not_of(' ') < raw.size() && raw[raw.find_first_not_of(' ')] == '\t')
            return make_error("invalid_yaml", "line " + std::to_string(no) + ": tabs cannot indent");
        const size_t first = raw.find_first_not_of(' ');
        if (first == std::string::npos) {
            if (e == text.size()) break;
            continue;
        }
        if (raw[first] == '#') {
            if (e == text.size()) break;
            continue;
        }
        Line l;
        l.no = no;
        l.indent = static_cast<int>(first);
        l.raw = raw;
        l.text = strip_comment(raw.substr(first));
        if (l.text.empty()) continue;
        lines.push_back(std::move(l));
        if (e == text.size()) break;
    }
    try {
        return Parser(std::move(lines), std::move(all), limits).document();
    } catch (const Fail& f) {
        return make_error("invalid_yaml", f.message);
    }
}

}  // namespace wb::skills
