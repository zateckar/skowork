#include "ui/markdown.h"

#include "md4c.h"
#include "ui/text16.h"

namespace wb::ui {
namespace {

std::u16string attr(const MD_ATTRIBUTE& a) { return to_utf16(std::string_view(a.text, a.size)); }

// "&#233;" / "&#xE9;" -> the character; named entities other than the common five stay literal.
std::u16string entity(std::string_view s) {
    if (s == "&amp;") return u"&";
    if (s == "&lt;") return u"<";
    if (s == "&gt;") return u">";
    if (s == "&quot;") return u"\"";
    if (s == "&apos;") return u"'";
    if (s == "&nbsp;") return u" ";
    if (s.size() >= 4 && s[1] == '#' && s.back() == ';') {
        const bool hex = s[2] == 'x' || s[2] == 'X';
        uint32_t cp = 0;
        bool ok = true;
        for (size_t i = hex ? 3 : 2; i + 1 < s.size() && ok; ++i) {
            const char c = s[i];
            uint32_t d = 0;
            if (c >= '0' && c <= '9') d = static_cast<uint32_t>(c - '0');
            else if (hex && c >= 'a' && c <= 'f') d = static_cast<uint32_t>(c - 'a' + 10);
            else if (hex && c >= 'A' && c <= 'F') d = static_cast<uint32_t>(c - 'A' + 10);
            else ok = false;
            cp = cp * (hex ? 16u : 10u) + d;
            if (cp > 0x10FFFF) ok = false;
        }
        if (!ok || cp == 0 || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
        std::u16string out;
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out.push_back(static_cast<char16_t>(0xD800 + (cp >> 10)));
            out.push_back(static_cast<char16_t>(0xDC00 + (cp & 0x3FF)));
        } else {
            out.push_back(static_cast<char16_t>(cp));
        }
        return out;
    }
    return to_utf16(s);
}

struct Parser {
    std::vector<Block> out;
    int cur = -1;  // index of the open leaf block
    struct List {
        bool ordered;
        unsigned next;
    };
    std::vector<List> lists;
    int quote = 0;
    bool item_fresh = false;  // the current item block has not received a paragraph yet
    bool in_cell = false;
    std::vector<std::vector<Cell>> rows;
    struct Open {
        unsigned style;
        uint32_t start;
        std::u16string url;
        bool image;
    };
    std::vector<Open> open;

    Block& add(BlockKind k) {
        Block b;
        b.kind = k;
        b.indent = static_cast<int>(lists.size());
        b.quote = quote;
        out.push_back(std::move(b));
        cur = static_cast<int>(out.size()) - 1;
        return out.back();
    }
    std::u16string& text() {
        if (in_cell) return rows.back().back().text;
        if (cur < 0) add(BlockKind::paragraph);
        return out[static_cast<size_t>(cur)].text;
    }
    std::vector<Span>& spans() {
        if (in_cell) return rows.back().back().spans;
        if (cur < 0) add(BlockKind::paragraph);
        return out[static_cast<size_t>(cur)].spans;
    }

    void enter_block(MD_BLOCKTYPE t, void* d) {
        switch (t) {
        case MD_BLOCK_QUOTE: ++quote; break;
        case MD_BLOCK_UL: lists.push_back({false, 0}); break;
        case MD_BLOCK_OL: lists.push_back({true, static_cast<MD_BLOCK_OL_DETAIL*>(d)->start}); break;
        case MD_BLOCK_LI: {
            auto* li = static_cast<MD_BLOCK_LI_DETAIL*>(d);
            Block& b = add(BlockKind::item);
            if (lists.empty()) lists.push_back({false, 0});
            List& l = lists.back();
            if (li->is_task) b.marker = (li->task_mark == ' ') ? u"☐" : u"☑";
            else if (l.ordered) b.marker = to_utf16(std::to_string(l.next++) + ".");
            else b.marker = u"•";
            item_fresh = true;
            break;
        }
        case MD_BLOCK_HR:
            add(BlockKind::rule);
            cur = -1;
            break;
        case MD_BLOCK_H: add(BlockKind::heading).level = static_cast<int>(static_cast<MD_BLOCK_H_DETAIL*>(d)->level); break;
        case MD_BLOCK_CODE: {
            auto* c = static_cast<MD_BLOCK_CODE_DETAIL*>(d);
            add(BlockKind::code).language = attr(c->lang);
            break;
        }
        case MD_BLOCK_HTML:
        case MD_BLOCK_P:
            if (cur >= 0 && item_fresh && out[static_cast<size_t>(cur)].kind == BlockKind::item &&
                out[static_cast<size_t>(cur)].text.empty())
                break;  // the item's first paragraph goes into the item block itself
            add(BlockKind::paragraph);
            break;
        case MD_BLOCK_TABLE: rows.clear(); break;
        case MD_BLOCK_TR: rows.emplace_back(); break;
        case MD_BLOCK_TH:
        case MD_BLOCK_TD:
            if (rows.empty()) rows.emplace_back();
            rows.back().emplace_back();
            rows.back().back().header = t == MD_BLOCK_TH;
            in_cell = true;
            break;
        default: break;
        }
    }
    void leave_block(MD_BLOCKTYPE t) {
        switch (t) {
        case MD_BLOCK_QUOTE: --quote; break;
        case MD_BLOCK_UL:
        case MD_BLOCK_OL:
            if (!lists.empty()) lists.pop_back();
            break;
        case MD_BLOCK_CODE:
            if (cur >= 0) {
                auto& s = out[static_cast<size_t>(cur)].text;
                while (!s.empty() && s.back() == u'\n') s.pop_back();
            }
            cur = -1;
            break;
        case MD_BLOCK_LI:
        case MD_BLOCK_H:
        case MD_BLOCK_P:
        case MD_BLOCK_HTML:
            item_fresh = false;
            cur = -1;
            break;
        case MD_BLOCK_TH:
        case MD_BLOCK_TD: in_cell = false; break;
        case MD_BLOCK_TABLE: {
            Block& b = add(BlockKind::table);
            b.rows = std::move(rows);
            rows.clear();
            cur = -1;
            break;
        }
        default: break;
        }
    }
    void enter_span(MD_SPANTYPE t, void* d) {
        const auto start = static_cast<uint32_t>(text().size());
        switch (t) {
        case MD_SPAN_EM: open.push_back({kItalic, start, {}, false}); break;
        case MD_SPAN_STRONG: open.push_back({kBold, start, {}, false}); break;
        case MD_SPAN_CODE: open.push_back({kCode, start, {}, false}); break;
        case MD_SPAN_DEL: open.push_back({kStrike, start, {}, false}); break;
        case MD_SPAN_A: open.push_back({kLink, start, attr(static_cast<MD_SPAN_A_DETAIL*>(d)->href), false}); break;
        case MD_SPAN_IMG: open.push_back({kLink, start, attr(static_cast<MD_SPAN_IMG_DETAIL*>(d)->src), true}); break;
        default: open.push_back({0, start, {}, false}); break;
        }
    }
    void leave_span() {
        if (open.empty()) return;
        Open o = std::move(open.back());
        open.pop_back();
        std::u16string& s = text();
        if (o.image) s += u" [" + o.url + u"]";  // never fetched: alternative text plus the link
        if (o.style && s.size() > o.start)
            spans().push_back({o.start, static_cast<uint32_t>(s.size()) - o.start, o.style, std::move(o.url)});
    }
    void on_text(MD_TEXTTYPE t, std::string_view s) {
        std::u16string& dst = text();
        switch (t) {
        case MD_TEXT_NULLCHAR: dst += u'�'; break;
        case MD_TEXT_BR: dst += u'\n'; break;
        case MD_TEXT_SOFTBR: dst += u' '; break;
        case MD_TEXT_ENTITY: dst += entity(s); break;
        default: dst += to_utf16(s); break;
        }
    }
};

}  // namespace

std::vector<Block> parse_markdown(std::string_view utf8) {
    Parser p;
    MD_PARSER parser{};
    parser.abi_version = 0;
    parser.flags = MD_DIALECT_GITHUB | MD_FLAG_NOHTML;
    parser.enter_block = [](MD_BLOCKTYPE t, void* d, void* u) {
        static_cast<Parser*>(u)->enter_block(t, d);
        return 0;
    };
    parser.leave_block = [](MD_BLOCKTYPE t, void*, void* u) {
        static_cast<Parser*>(u)->leave_block(t);
        return 0;
    };
    parser.enter_span = [](MD_SPANTYPE t, void* d, void* u) {
        static_cast<Parser*>(u)->enter_span(t, d);
        return 0;
    };
    parser.leave_span = [](MD_SPANTYPE, void*, void* u) {
        static_cast<Parser*>(u)->leave_span();
        return 0;
    };
    parser.text = [](MD_TEXTTYPE t, const MD_CHAR* s, MD_SIZE n, void* u) {
        static_cast<Parser*>(u)->on_text(t, std::string_view(s, n));
        return 0;
    };
    md_parse(utf8.data(), static_cast<MD_SIZE>(utf8.size()), &parser, &p);
    return std::move(p.out);
}

namespace {

bool md_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
bool md_alnum(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || static_cast<unsigned char>(c) >= 0x80;
}

// The offset where the last paragraph outside fenced code starts, or npos when the text ends inside a fenced code
// block (md4c runs an open fence to the end by itself).
size_t last_paragraph(std::string_view s) {
    size_t start = 0, line = 0;
    char fence = 0;
    size_t fence_len = 0;
    while (line <= s.size()) {
        const size_t end = std::min(s.find('\n', line), s.size());
        const std::string_view l = s.substr(line, end - line);
        size_t i = 0;
        while (i < l.size() && i < 3 && l[i] == ' ') ++i;
        const char c = i < l.size() ? l[i] : 0;
        size_t run = 0;
        while (i + run < l.size() && l[i + run] == c) ++run;
        if ((c == '`' || c == '~') && run >= 3) {
            if (!fence) {
                fence = c;
                fence_len = run;
            } else if (c == fence && run >= fence_len && l.find_first_not_of(" \t\r", i + run) == std::string_view::npos) {
                fence = 0;
                start = end + 1;
            }
        } else if (!fence && l.find_first_not_of(" \t\r") == std::string_view::npos) {
            start = end + 1;  // a blank line ends the paragraph
        }
        if (end >= s.size()) break;
        line = end + 1;
    }
    if (fence) return std::string_view::npos;
    return std::min(start, s.size());
}

// A link at the end of the paragraph that is not complete: "[text](partial" or "[text]" with nothing after it.
// Returns the paragraph with that link replaced by its text.
std::string cut_open_link(std::string p) {
    // Find the last unescaped '[' outside code spans.
    size_t open = std::string::npos;
    size_t code = 0;  // the backtick run length of an open code span
    for (size_t i = 0; i < p.size(); ++i) {
        const char c = p[i];
        if (c == '\\' && !code) {
            ++i;
            continue;
        }
        if (c == '`') {
            size_t run = 1;
            while (i + run < p.size() && p[i + run] == '`') ++run;
            if (!code) code = run;
            else if (run == code) code = 0;
            i += run - 1;
            continue;
        }
        if (!code && c == '[') open = i;
    }
    if (open == std::string::npos) return p;
    const size_t close = p.find(']', open + 1);
    size_t from = open;
    if (from > 0 && p[from - 1] == '!') --from;  // an image: its alternative text, as for a link
    if (close == std::string::npos) return p;  // "[2": brackets stay literal until it is a link
    const std::string label = p.substr(open + 1, close - open - 1);
    // "[text]": a "(" may follow. A number in brackets is a Citation marker ([2]) and stays as it is.
    const bool number = !label.empty() && label.find_first_not_of("0123456789, ") == std::string::npos;
    if (close + 1 == p.size()) return number ? p : p.substr(0, from) + label;
    if (p[close + 1] == '(' && p.find(')', close + 2) == std::string::npos) return p.substr(0, from) + label;
    return p;
}

}  // namespace

std::string close_streaming_markdown(std::string_view utf8) {
    const size_t start = last_paragraph(utf8);
    if (start == std::string_view::npos || start >= utf8.size()) return std::string(utf8);
    std::string p = cut_open_link(std::string(utf8.substr(start)));
    struct Open {
        char c;
        size_t len;
    };
    std::vector<Open> stack;
    size_t code = 0;
    size_t dangling = std::string::npos;  // the start of a delimiter run that neither opened nor closed
    size_t code_start = 0;                // where the open code span's backticks start
    for (size_t i = 0; i < p.size(); ++i) {
        const char c = p[i];
        if (code) {
            if (c == '`') {
                size_t run = 1;
                while (i + run < p.size() && p[i + run] == '`') ++run;
                if (run == code) code = 0;
                i += run - 1;
            }
            continue;
        }
        if (c == '\\') {
            ++i;
            continue;
        }
        if (c == '`') {
            size_t run = 1;
            while (i + run < p.size() && p[i + run] == '`') ++run;
            code = run;
            code_start = i;
            i += run - 1;
            continue;
        }
        if (c != '*' && c != '_' && c != '~') continue;
        size_t run = 1;
        while (i + run < p.size() && p[i + run] == c) ++run;
        const char before = i > 0 ? p[i - 1] : '\n';
        const char after = i + run < p.size() ? p[i + run] : '\n';
        bool can_open = !md_space(after) && i + run < p.size();
        bool can_close = !md_space(before) && i > 0;
        if (c == '_') {  // intraword underscores are literal
            can_open = can_open && !md_alnum(before);
            can_close = can_close && !md_alnum(after);
        }
        if (c == '~' && run > 2) can_open = can_close = false;
        size_t left = run;
        if (can_close)
            while (left > 0 && !stack.empty() && stack.back().c == c) {
                Open& top = stack.back();
                const size_t take = std::min(left, top.len);
                top.len -= take;
                left -= take;
                if (top.len == 0) stack.pop_back();
            }
        if (left > 0 && can_open && left == run) stack.push_back({c, run});
        dangling = left == run && !(can_open && left > 0) ? i : std::string::npos;
        i += run - 1;
    }
    // A delimiter run at the very end that did nothing yet is held back: it may open or close once more text arrives.
    // Closers go before trailing white space, where they still close (a delimiter after a space cannot close).
    size_t tail = p.size();
    while (tail > 0 && md_space(p[tail - 1])) --tail;
    if (code && code_start + code >= tail) {  // backticks with nothing after them yet: held back
        code = 0;
        tail = code_start;
        while (tail > 0 && md_space(p[tail - 1])) --tail;
    }
    if (!code && dangling != std::string::npos && dangling < tail &&
        p.find_first_not_of(p[dangling], dangling) >= tail) {
        tail = dangling;
        while (tail > 0 && md_space(p[tail - 1])) --tail;
    }
    std::string closers;
    if (code) closers += std::string(code, '`');
    for (auto it = stack.rbegin(); it != stack.rend(); ++it) closers += std::string(it->len, it->c);
    if (closers.empty() && tail == p.size() && p == utf8.substr(start)) return std::string(utf8);
    p.resize(tail);
    p += closers;
    return std::string(utf8.substr(0, start)) + p;
}

std::u16string plain_text(const std::vector<Block>& blocks) {
    std::u16string out;
    for (const Block& b : blocks) {
        if (!out.empty()) out += u'\n';
        switch (b.kind) {
        case BlockKind::rule: out += u"---"; break;
        case BlockKind::item: out += std::u16string(static_cast<size_t>(b.indent > 0 ? b.indent - 1 : 0) * 2, u' ') + b.marker + u' ' + b.text; break;
        case BlockKind::table:
            for (size_t r = 0; r < b.rows.size(); ++r) {
                if (r) out += u'\n';
                for (size_t c = 0; c < b.rows[r].size(); ++c) {
                    if (c) out += u'\t';
                    out += b.rows[r][c].text;
                }
            }
            break;
        default: out += b.text; break;
        }
    }
    return out;
}

}  // namespace wb::ui
