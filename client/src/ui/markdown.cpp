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
