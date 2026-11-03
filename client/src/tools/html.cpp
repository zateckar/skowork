#include "tools/html.h"

#include "base/strings.h"

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

namespace wb::tools {

namespace {

void put_utf8(std::string& out, char32_t c) {
    if (c == 0 || (c >= 0xD800 && c <= 0xDFFF) || c > 0x10FFFF) c = 0xFFFD;
    if (c < 0x80) {
        out += static_cast<char>(c);
    } else if (c < 0x800) {
        out += static_cast<char>(0xC0 | (c >> 6));
        out += static_cast<char>(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
        out += static_cast<char>(0xE0 | (c >> 12));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (c >> 18));
        out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    }
}

constexpr std::pair<const char*, char32_t> kNamed[] = {
    {"amp", '&'},      {"lt", '<'},        {"gt", '>'},        {"quot", '"'},      {"apos", '\''},     {"nbsp", ' '},
    {"copy", 0xA9},    {"reg", 0xAE},      {"trade", 0x2122},  {"hellip", 0x2026}, {"mdash", 0x2014},  {"ndash", 0x2013},
    {"lsquo", 0x2018}, {"rsquo", 0x2019},  {"ldquo", 0x201C},  {"rdquo", 0x201D},  {"bdquo", 0x201E},  {"laquo", 0xAB},
    {"raquo", 0xBB},   {"euro", 0x20AC},   {"bull", 0x2022},   {"middot", 0xB7},   {"times", 0xD7},    {"divide", 0xF7},
    {"deg", 0xB0},     {"plusmn", 0xB1},   {"sect", 0xA7},     {"para", 0xB6},     {"shy", 0xAD},      {"auml", 0xE4},
    {"ouml", 0xF6},    {"uuml", 0xFC},     {"Auml", 0xC4},     {"Ouml", 0xD6},     {"Uuml", 0xDC},     {"szlig", 0xDF},
    {"aacute", 0xE1},  {"eacute", 0xE9},   {"iacute", 0xED},   {"oacute", 0xF3},   {"uacute", 0xFA},   {"yacute", 0xFD},
    {"Aacute", 0xC1},  {"Eacute", 0xC9},   {"Iacute", 0xCD},   {"Oacute", 0xD3},   {"Uacute", 0xDA},   {"Yacute", 0xDD},
    {"egrave", 0xE8},  {"agrave", 0xE0},   {"ccedil", 0xE7},   {"ntilde", 0xF1},   {"scaron", 0x161},  {"Scaron", 0x160},
    {"zcaron", 0x17E}, {"Zcaron", 0x17D},  {"ccaron", 0x10D},  {"Ccaron", 0x10C},  {"rcaron", 0x159},  {"Rcaron", 0x158},
    {"ecaron", 0x11B}, {"Ecaron", 0x11A},  {"ncaron", 0x148},  {"Ncaron", 0x147},  {"dcaron", 0x10F},  {"Dcaron", 0x10E},
    {"tcaron", 0x165}, {"Tcaron", 0x164},  {"uring", 0x16F},   {"Uring", 0x16E},   {"larr", 0x2190},   {"rarr", 0x2192},
    {"zwj", 0x200D},   {"zwnj", 0x200C},   {"ensp", ' '},      {"emsp", ' '},      {"thinsp", ' '},
};

bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }
bool is_alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool is_name_char(char c) { return is_alpha(c) || (c >= '0' && c <= '9') || c == '-' || c == ':' || c == '_'; }

char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

// Case-insensitive search for `needle` (lower case) from `from`.
size_t ifind(std::string_view h, std::string_view needle, size_t from) {
    if (needle.empty() || h.size() < needle.size()) return std::string_view::npos;
    for (size_t i = from; i + needle.size() <= h.size(); ++i) {
        size_t k = 0;
        while (k < needle.size() && lower(h[i + k]) == needle[k]) ++k;
        if (k == needle.size()) return i;
    }
    return std::string_view::npos;
}

struct Tag {
    std::string name;  // lower case
    bool end = false;
    bool self_closing = false;
    std::vector<std::pair<std::string, std::string>> attrs;  // lower-case names, decoded values
    [[nodiscard]] const std::string* attr(std::string_view n) const {
        for (const auto& [k, v] : attrs)
            if (k == n) return &v;
        return nullptr;
    }
};

// Parses a tag starting at h[i] == '<'. Returns the position after '>', npos when this is not a tag, or h.size() for a
// tag the input ends inside (as in browsers, the rest is not text).
size_t parse_tag(std::string_view h, size_t i, Tag& t) {
    size_t p = i + 1;
    if (p < h.size() && h[p] == '/') {
        t.end = true;
        ++p;
    }
    if (p >= h.size() || !is_alpha(h[p])) return std::string_view::npos;
    while (p < h.size() && is_name_char(h[p])) t.name += lower(h[p++]);
    for (;;) {
        while (p < h.size() && (is_space(h[p]) || h[p] == '/')) {
            if (h[p] == '/') t.self_closing = true;
            ++p;
        }
        if (p >= h.size()) return h.size();
        if (h[p] == '>') return p + 1;
        t.self_closing = false;
        std::string name;
        while (p < h.size() && !is_space(h[p]) && h[p] != '=' && h[p] != '>' && h[p] != '/') name += lower(h[p++]);
        while (p < h.size() && is_space(h[p])) ++p;
        std::string value;
        if (p < h.size() && h[p] == '=') {
            ++p;
            while (p < h.size() && is_space(h[p])) ++p;
            if (p < h.size() && (h[p] == '"' || h[p] == '\'')) {
                const char q = h[p++];
                const size_t close = h.find(q, p);
                if (close == std::string_view::npos) return h.size();
                value = decode_entities(h.substr(p, close - p));
                p = close + 1;
            } else {
                const size_t start = p;
                while (p < h.size() && !is_space(h[p]) && h[p] != '>') ++p;
                value = decode_entities(h.substr(start, p - start));
            }
        }
        if (!name.empty() && t.attrs.size() < 64) t.attrs.emplace_back(std::move(name), std::move(value));
    }
}

bool one_of(std::string_view n, std::initializer_list<std::string_view> names) {
    for (auto x : names)
        if (n == x) return true;
    return false;
}

bool is_void(std::string_view n) {
    return one_of(n, {"area", "base", "br", "col", "embed", "hr", "img", "input", "link", "meta", "param", "source", "track", "wbr"});
}
// Elements whose content is never text for the reader.
bool is_raw_skip(std::string_view n) {
    return one_of(n, {"script", "style", "noscript", "template", "svg", "math", "iframe", "object", "canvas", "select", "textarea"});
}
bool is_block(std::string_view n) {
    return one_of(n, {"div", "section", "article", "header", "footer", "nav", "main", "aside", "form", "fieldset", "figure",
                      "figcaption", "address", "dl", "dt", "dd", "ul", "ol", "table", "thead", "tbody", "tfoot", "caption",
                      "details", "summary", "center", "body", "html"});
}

bool hidden(const Tag& t) {
    if (t.attr("hidden")) return true;
    if (const auto* a = t.attr("aria-hidden"); a && to_lower(*a) == "true") return true;
    if (const auto* s = t.attr("style")) {
        std::string compact;
        for (const char c : *s)
            if (!is_space(c)) compact += lower(c);
        if (compact.find("display:none") != std::string::npos || compact.find("visibility:hidden") != std::string::npos) return true;
    }
    if (t.name == "input") return true;
    return false;
}

std::string resolve(std::string_view base, const std::string& href) {
    if (href.empty() || base.empty()) return href;
    // Any "scheme:" target is absolute already.
    size_t s = 0;
    while (s < href.size() && (is_alpha(href[s]) || (s > 0 && ((href[s] >= '0' && href[s] <= '9') || href[s] == '+' || href[s] == '-' || href[s] == '.'))))
        ++s;
    if (s > 0 && s < href.size() && href[s] == ':') return href;
    const size_t scheme_end = base.find("://");
    if (scheme_end == std::string_view::npos) return href;
    const size_t path_start = base.find('/', scheme_end + 3);
    const std::string origin(base.substr(0, path_start == std::string_view::npos ? base.size() : path_start));
    if (starts_with(href, "//")) return std::string(base.substr(0, scheme_end + 1)) + href;
    if (href[0] == '/') return origin + href;
    if (href[0] == '?' || href[0] == '#') {
        const size_t cut = base.find_first_of(href[0] == '?' ? "?#" : "#");
        return std::string(base.substr(0, cut)) + href;
    }
    std::string path = path_start == std::string_view::npos ? "/" : std::string(base.substr(path_start));
    path = path.substr(0, path.find_first_of("?#"));
    path = path.substr(0, path.rfind('/') + 1);
    return origin + path + href;
}

class Writer {
public:
    std::string out;

    void text(std::string_view t, bool pre) {
        for (const char c : t) {
            if (!pre && is_space(c)) {
                space_ = !out.empty() && out.back() != '\n';
                continue;
            }
            if (space_) {
                out += ' ';
                space_ = false;
            }
            if (pre && c == '\r') continue;
            out += c;
        }
    }
    void raw(std::string_view s) {
        if (space_ && !out.empty() && out.back() != '\n') out += ' ';
        space_ = false;
        out += s;
    }
    // Ends the current line and leaves `lines` - 1 empty lines (1: new line, 2: paragraph break).
    void breaks(int lines) {
        space_ = false;
        while (!out.empty() && out.back() == ' ') out.pop_back();
        if (out.empty()) return;
        int have = 0;
        for (size_t i = out.size(); i > 0 && out[i - 1] == '\n' && have < lines; --i) ++have;
        while (have++ < lines) out += '\n';
    }
    void drop_space() { space_ = false; }
    [[nodiscard]] bool space_pending() const { return space_; }

private:
    bool space_ = false;
};

}  // namespace

std::string decode_entities(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        if (s[i] != '&') {
            out += s[i++];
            continue;
        }
        size_t j = i + 1;
        if (j < s.size() && s[j] == '#') {
            ++j;
            const bool hex = j < s.size() && (s[j] == 'x' || s[j] == 'X');
            if (hex) ++j;
            std::uint32_t v = 0;
            size_t digits = 0;
            while (j < s.size() && digits < 8) {
                const char c = s[j];
                int d = -1;
                if (c >= '0' && c <= '9') d = c - '0';
                else if (hex && c >= 'a' && c <= 'f') d = c - 'a' + 10;
                else if (hex && c >= 'A' && c <= 'F') d = c - 'A' + 10;
                if (d < 0) break;
                v = v * (hex ? 16 : 10) + static_cast<std::uint32_t>(d);
                ++j;
                ++digits;
            }
            if (digits == 0) {
                out += s[i++];
                continue;
            }
            if (j < s.size() && s[j] == ';') ++j;
            put_utf8(out, v > 0x10FFFF ? 0xFFFD : static_cast<char32_t>(v));
            i = j;
            continue;
        }
        size_t k = j;
        while (k < s.size() && k - j < 10 && is_name_char(s[k])) ++k;
        const std::string_view name = s.substr(j, k - j);
        bool done = false;
        for (const auto& [n, cp] : kNamed) {
            if (name == n) {
                put_utf8(out, cp == 0xAD ? 0 : cp);
                if (cp == 0xAD) out.pop_back();  // soft hyphen: nothing visible
                i = k < s.size() && s[k] == ';' ? k + 1 : k;
                done = true;
                break;
            }
        }
        if (!done) out += s[i++];
    }
    return out;
}

HtmlText html_to_text(std::string_view h, std::string_view base_url) {
    HtmlText result;
    Writer w;
    int pre = 0;
    std::string hidden_tag;  // the element whose hidden subtree is being skipped
    int hidden_depth = 0;
    std::vector<std::pair<size_t, std::string>> links;  // '[' position and target of open links
    int cells = 0;
    int list_depth = 0;

    for (size_t i = 0; i < h.size();) {
        if (h[i] != '<') {
            const size_t next = h.find('<', i);
            const size_t end = next == std::string_view::npos ? h.size() : next;
            if (hidden_depth == 0) w.text(decode_entities(h.substr(i, end - i)), pre > 0);
            i = end;
            continue;
        }
        // Comments, doctype, processing instructions and CDATA.
        if (h.compare(i, 4, "<!--") == 0) {
            const size_t close = h.find("-->", i + 4);
            i = close == std::string_view::npos ? h.size() : close + 3;
            continue;
        }
        if (h.compare(i, 9, "<![CDATA[") == 0) {
            const size_t close = h.find("]]>", i + 9);
            const size_t end = close == std::string_view::npos ? h.size() : close;
            if (hidden_depth == 0) w.text(h.substr(i + 9, end - i - 9), pre > 0);
            i = close == std::string_view::npos ? h.size() : close + 3;
            continue;
        }
        if (i + 1 < h.size() && (h[i + 1] == '!' || h[i + 1] == '?')) {
            const size_t close = h.find('>', i);
            i = close == std::string_view::npos ? h.size() : close + 1;
            continue;
        }
        Tag t;
        const size_t after = parse_tag(h, i, t);
        if (after == std::string_view::npos) {
            if (hidden_depth == 0) w.text("<", pre > 0);
            ++i;
            continue;
        }
        i = after;

        // Raw-text elements: skip to their end tag (their content is not markup).
        if (!t.end && (is_raw_skip(t.name) || t.name == "title")) {
            const std::string close = "</" + t.name;
            size_t end = ifind(h, close, i);
            if (end == std::string_view::npos) end = h.size();
            if (t.name == "title" && result.title.empty() && hidden_depth == 0) {
                Writer tw;
                tw.text(decode_entities(h.substr(i, end - i)), false);
                result.title = trim(tw.out);
            }
            const size_t gt = end < h.size() ? h.find('>', end) : std::string_view::npos;
            i = gt == std::string_view::npos ? h.size() : gt + 1;
            continue;
        }

        // Hidden subtrees: count nesting of the same element until it closes.
        if (hidden_depth > 0) {
            if (t.name == hidden_tag && !is_void(t.name) && !t.self_closing) hidden_depth += t.end ? -1 : 1;
            continue;
        }
        if (!t.end && hidden(t)) {
            if (!is_void(t.name) && !t.self_closing) {
                hidden_tag = t.name;
                hidden_depth = 1;
            }
            continue;
        }

        const std::string& n = t.name;
        if (n.size() == 2 && n[0] == 'h' && n[1] >= '1' && n[1] <= '6') {
            w.breaks(2);
            if (!t.end) w.raw(std::string(static_cast<size_t>(n[1] - '0'), '#') + " ");
        } else if (n == "p" || n == "blockquote") {
            w.breaks(2);
        } else if (n == "pre") {
            if (t.end) {
                pre = pre > 0 ? pre - 1 : 0;
                w.breaks(1);
                w.raw("```");
                w.breaks(2);
            } else {
                ++pre;
                w.breaks(2);
                w.raw("```\n");
            }
        } else if (n == "br") {
            if (w.out.empty() || w.out.back() != '\n') w.out += '\n';
            w.drop_space();
        } else if (n == "hr") {
            w.breaks(2);
            w.raw("---");
            w.breaks(2);
        } else if (n == "li") {
            w.breaks(1);
            if (!t.end) w.raw(std::string(static_cast<size_t>(std::max(0, list_depth - 1) * 2), ' ') + "- ");
        } else if (n == "ul" || n == "ol") {
            list_depth = std::max(0, list_depth + (t.end ? -1 : 1));
            w.breaks(1);
        } else if (n == "tr") {
            w.breaks(1);
            cells = 0;
        } else if (n == "td" || n == "th") {
            if (!t.end && cells++ > 0) w.raw(" | ");
        } else if (n == "img" && !t.end) {
            if (const auto* alt = t.attr("alt"); alt && !trim(*alt).empty()) w.raw("[image: " + trim(*alt) + "]");
        } else if (n == "a") {
            if (!t.end) {
                const auto* href = t.attr("href");
                const std::string raw_href = href ? trim(*href) : std::string();
                const std::string low = to_lower(raw_href.substr(0, 11));
                std::string target;
                if (!raw_href.empty() && raw_href[0] != '#' && !starts_with(low, "javascript:") && !starts_with(low, "data:") &&
                    !starts_with(low, "vbscript:"))
                    target = resolve(base_url, raw_href);
                if (!target.empty() && links.size() < 64) {
                    w.raw("[");
                    links.emplace_back(w.out.size() - 1, std::move(target));
                }
            } else if (!links.empty()) {
                auto [at, target] = std::move(links.back());
                links.pop_back();
                if (at < w.out.size()) {
                    std::string label = trim(std::string_view(w.out).substr(at + 1));
                    if (label.empty()) {
                        w.out.erase(at);
                    } else {
                        // "[label ](x)" becomes "[label](x) ": whitespace at the end of the label moves after the link.
                        bool spaced = w.space_pending();
                        while (w.out.back() == ' ' || w.out.back() == '\n') {
                            w.out.pop_back();
                            spaced = true;
                        }
                        w.drop_space();
                        w.raw("](" + target + ")");
                        if (spaced) w.text(" ", false);
                    }
                }
            }
        } else if (is_block(n)) {
            w.breaks(1);
        }
    }
    // Normalise: trailing spaces per line, at most one empty line in a row.
    std::string out;
    out.reserve(w.out.size());
    for (const char c : w.out) {
        if (c == '\n') {
            while (!out.empty() && out.back() == ' ') out.pop_back();
            const bool two = out.size() >= 2 && out[out.size() - 1] == '\n' && out[out.size() - 2] == '\n';
            if (!out.empty() && !two) out += '\n';
            continue;
        }
        out += c;
    }
    while (!out.empty() && (out.back() == '\n' || out.back() == ' ')) out.pop_back();
    if (!out.empty()) out += '\n';
    result.text = std::move(out);
    return result;
}

}  // namespace wb::tools
