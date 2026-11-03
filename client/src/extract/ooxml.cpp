#include "extract/extract.h"

#include <miniz.h>
#include <pugixml.hpp>

#include <algorithm>
#include <cstring>
#include <functional>
#include <map>
#include <vector>

namespace wb::extract {

namespace {

// ---- Zip container -------------------------------------------------------------------------------------------------

class Zip {
public:
    explicit Zip(std::string_view bytes) {
        ok_ = mz_zip_reader_init_mem(&zip_, bytes.data(), bytes.size(), 0) && mz_zip_reader_get_num_files(&zip_) <= kMaxEntries;
    }
    ~Zip() { mz_zip_reader_end(&zip_); }
    Zip(const Zip&) = delete;
    Zip& operator=(const Zip&) = delete;

    [[nodiscard]] bool ok() const { return ok_; }

    [[nodiscard]] bool has(const std::string& name) { return ok_ && mz_zip_reader_locate_file(&zip_, name.c_str(), nullptr, 0) >= 0; }

    Result<std::string> read(const std::string& name) {
        if (!ok_) return make_error("malformed", "not a zip package");
        const int index = mz_zip_reader_locate_file(&zip_, name.c_str(), nullptr, 0);
        if (index < 0) return make_error("malformed", "the package has no part " + name);
        mz_zip_archive_file_stat st{};
        if (!mz_zip_reader_file_stat(&zip_, static_cast<mz_uint>(index), &st)) return make_error("malformed", "unreadable zip entry " + name);
        if (st.m_uncomp_size > kMaxPartBytes) return make_error("too_large", "the part " + name + " is larger than 64 MB");
        if (read_total_ + st.m_uncomp_size > kMaxTotalBytes) return make_error("too_large", "the document expands to more than 256 MB");
        std::string out(static_cast<size_t>(st.m_uncomp_size), '\0');
        // The declared size is the buffer size: an entry that inflates to more fails instead of growing.
        if (!out.empty() && !mz_zip_reader_extract_to_mem(&zip_, static_cast<mz_uint>(index), out.data(), out.size(), 0))
            return make_error("malformed", "the part " + name + " cannot be decompressed");
        read_total_ += out.size();
        return out;
    }

private:
    mz_zip_archive zip_{};
    bool ok_ = false;
    size_t read_total_ = 0;
};

// ---- XML helpers ---------------------------------------------------------------------------------------------------

std::string_view local(const char* name) {
    std::string_view n(name);
    const auto colon = n.find(':');
    return colon == std::string_view::npos ? n : n.substr(colon + 1);
}

// The value of the attribute whose local name is `name` (any prefix).
std::string attr(const pugi::xml_node& n, std::string_view name) {
    for (const auto& a : n.attributes())
        if (local(a.name()) == name) return a.value();
    return {};
}

// The relationship reference of an element (r:id with any prefix; not a plain `id` attribute).
std::string rel_id(const pugi::xml_node& n) {
    for (const auto& a : n.attributes()) {
        const std::string_view name(a.name());
        if (name.find(':') != std::string_view::npos && local(a.name()) == "id") return a.value();
    }
    return {};
}

Result<pugi::xml_document*> parse(pugi::xml_document& doc, const std::string& xml, const std::string& part) {
    const auto r = doc.load_buffer(xml.data(), xml.size(), pugi::parse_default | pugi::parse_ws_pcdata_single, pugi::encoding_auto);
    if (!r) return make_error("malformed", "the part " + part + " is not well-formed XML (" + r.description() + ")");
    return &doc;
}

// Pre-order walk without recursion (documents can nest deeply). `enter` returns false to skip the children; `leave` is
// called after the children.
void walk(const pugi::xml_node& root, const std::function<bool(const pugi::xml_node&)>& enter,
          const std::function<void(const pugi::xml_node&)>& leave) {
    pugi::xml_node n = root.first_child();
    while (n) {
        if (enter(n) && n.first_child()) {
            n = n.first_child();
            continue;
        }
        for (;;) {
            leave(n);
            if (n.next_sibling()) {
                n = n.next_sibling();
                break;
            }
            n = n.parent();
            if (!n || n == root) return;
        }
    }
}

// All text of the `t` elements under `n` (shared strings, inline strings).
std::string text_of(const pugi::xml_node& n) {
    std::string s;
    walk(
        n,
        [&](const pugi::xml_node& c) {
            const auto l = local(c.name());
            if (l == "t") {
                s += c.child_value();
                return false;
            }
            return l != "rPh" && l != "phoneticPr";  // phonetic guides are not text
        },
        [](const pugi::xml_node&) {});
    return s;
}

// Relationship id -> target, from a .rels part.
std::map<std::string, std::string> relationships(Zip& zip, const std::string& rels_part) {
    std::map<std::string, std::string> out;
    auto xml = zip.read(rels_part);
    if (!xml) return out;
    pugi::xml_document doc;
    if (!parse(doc, *xml, rels_part)) return out;
    for (const auto& r : doc.document_element().children())
        if (local(r.name()) == "Relationship") out[attr(r, "Id")] = attr(r, "Target");
    return out;
}

// The part a relationship target names, relative to the folder of the part that holds the relationship.
std::string resolve_part(const std::string& from_part, const std::string& target) {
    std::string path = !target.empty() && target[0] == '/' ? target.substr(1) : from_part.substr(0, from_part.rfind('/') + 1) + target;
    std::vector<std::string> segs;
    size_t start = 0;
    while (start <= path.size()) {
        const size_t slash = path.find('/', start);
        const std::string seg = path.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        if (seg == "..") {
            if (!segs.empty()) segs.pop_back();
        } else if (!seg.empty() && seg != ".") {
            segs.push_back(seg);
        }
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    std::string out;
    for (const auto& s : segs) out += (out.empty() ? "" : "/") + s;
    return out;
}

// The main part (officeDocument relationship of the package), or `fallback`.
std::string main_part(Zip& zip, const std::string& fallback) {
    auto xml = zip.read("_rels/.rels");
    if (xml) {
        pugi::xml_document doc;
        if (parse(doc, *xml, "_rels/.rels")) {
            for (const auto& r : doc.document_element().children()) {
                const std::string type = attr(r, "Type");
                if (local(r.name()) == "Relationship" && type.size() >= 15 && type.compare(type.size() - 15, 15, "/officeDocument") == 0)
                    return resolve_part("", attr(r, "Target"));
            }
        }
    }
    return fallback;
}

std::string rels_of(const std::string& part) {
    const auto slash = part.rfind('/');
    return (slash == std::string::npos ? std::string{} : part.substr(0, slash + 1)) + "_rels/" + part.substr(slash + 1) + ".rels";
}

// ---- Shared text writer --------------------------------------------------------------------------------------------

// Paragraphs and tables of WordprocessingML and DrawingML into lines.
class Writer {
public:
    std::string out;

    void begin_paragraph(std::string prefix) {
        para_.clear();
        prefix_ = std::move(prefix);
    }
    void text(std::string_view t) { para_ += t; }
    void end_paragraph() {
        std::string p = trim(para_);
        para_.clear();
        if (cells_.empty()) {
            if (p.empty()) return;
            if (!prefix_.empty() && prefix_[0] == '#' && !out.empty() && out.back() == '\n' && (out.size() < 2 || out[out.size() - 2] != '\n'))
                out += '\n';  // a blank line before a heading
            append(prefix_ + p + "\n");
        } else if (!p.empty()) {
            std::string& cell = cells_.back().back();
            cell += (cell.empty() ? "" : " ") + p;
        }
    }
    void begin_table() { cells_.emplace_back(); }
    void begin_row() {
        if (!cells_.empty()) cells_.back().clear();
    }
    void begin_cell() {
        if (!cells_.empty()) cells_.back().emplace_back();
    }
    void end_row() {
        if (cells_.empty()) return;
        std::vector<std::string> row = std::move(cells_.back());
        cells_.back().clear();
        std::string line;
        for (size_t i = 0; i < row.size(); ++i) line += (i ? " | " : "") + row[i];
        if (trim(line).empty()) return;
        if (cells_.size() > 1) {
            // A nested table becomes text of the enclosing cell.
            cells_.pop_back();
            text(line);
            cells_.emplace_back();
            return;
        }
        append(line + "\n");
    }
    void end_table() {
        if (cells_.empty()) return;
        cells_.pop_back();
        if (cells_.empty()) append("\n");
    }
    [[nodiscard]] bool too_large() const { return too_large_; }

    void append(std::string_view s) {
        if (out.size() + s.size() > kMaxTextBytes) {
            too_large_ = true;
            return;
        }
        out += s;
    }
    static std::string trim(std::string_view s) {
        size_t b = 0, e = s.size();
        while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\n' || s[b] == '\r')) ++b;
        while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\n' || s[e - 1] == '\r')) --e;
        return std::string(s.substr(b, e - b));
    }

private:
    std::string para_, prefix_;
    std::vector<std::vector<std::string>> cells_;  // one row of cells per open table
    bool too_large_ = false;
};

// The shared walk for w: and a: content: paragraphs (p), runs of text (t), tabs and breaks, tables (tbl/tr/tc).
void write_body(const pugi::xml_node& root, Writer& w, const std::function<std::string(const pugi::xml_node& p)>& prefix_of) {
    walk(
        root,
        [&](const pugi::xml_node& n) {
            const auto l = local(n.name());
            if (l == "p") {
                w.begin_paragraph(prefix_of(n));
            } else if (l == "t") {
                w.text(n.child_value());
                return false;
            } else if (l == "tab") {
                w.text("\t");
            } else if (l == "br" || l == "cr") {
                w.text(" ");
            } else if (l == "tbl") {
                w.begin_table();
            } else if (l == "tr") {
                w.begin_row();
            } else if (l == "tc") {
                w.begin_cell();
            } else if (l == "delText" || l == "instrText" || l == "pPr" || l == "rPr" || l == "sectPr" || l == "fldData" || l == "tblPr" ||
                       l == "tcPr" || l == "trPr" || l == "tblGrid") {
                return false;  // deleted text, field codes and properties are not content
            }
            return true;
        },
        [&](const pugi::xml_node& n) {
            const auto l = local(n.name());
            if (l == "p") w.end_paragraph();
            else if (l == "tr") w.end_row();
            else if (l == "tbl") w.end_table();
        });
}

Result<std::string> finish(Writer& w) {
    if (w.too_large()) return make_error("too_large", "the extracted text is larger than 32 MB");
    std::string out = std::move(w.out);
    while (!out.empty() && (out.back() == '\n' || out.back() == ' ')) out.pop_back();
    if (!out.empty()) out += '\n';
    return out;
}

// ---- Column references ----------------------------------------------------------------------------------------------

// "BC12" -> 54 (0-based column), or -1.
long column_of(std::string_view ref) {
    long col = 0;
    size_t i = 0;
    while (i < ref.size() && i < 4 && ref[i] >= 'A' && ref[i] <= 'Z') col = col * 26 + (ref[i++] - 'A' + 1);
    return i == 0 ? -1 : col - 1;
}

}  // namespace

Result<std::string> extract_docx(std::string_view bytes) {
    Zip zip(bytes);
    if (!zip.ok()) return make_error("malformed", "not a docx document (no zip package)");
    const std::string part = main_part(zip, "word/document.xml");
    auto xml = zip.read(part);
    if (!xml) return xml.error();
    pugi::xml_document doc;
    if (auto p = parse(doc, *xml, part); !p) return p.error();
    Writer w;
    write_body(doc.document_element(), w, [](const pugi::xml_node& p) -> std::string {
        for (const auto& ppr : p.children()) {
            if (local(ppr.name()) != "pPr") continue;
            for (const auto& c : ppr.children()) {
                const auto l = local(c.name());
                if (l == "pStyle") {
                    std::string v = attr(c, "val");
                    std::string low;
                    for (const char ch : v) low += static_cast<char>(ch >= 'A' && ch <= 'Z' ? ch - 'A' + 'a' : ch);
                    if (low == "title") return "# ";
                    if (low.rfind("heading", 0) == 0) {
                        const auto digit = low.find_first_of("123456789");
                        const int level = digit == std::string::npos ? 1 : low[digit] - '0';
                        return std::string(static_cast<size_t>(std::min(level, 6)), '#') + " ";
                    }
                    if (low.find("list") != std::string::npos) return "- ";
                }
                if (l == "numPr") return "- ";
                if (l == "outlineLvl") {
                    const std::string v = attr(c, "val");
                    if (v.size() == 1 && v[0] >= '0' && v[0] <= '5') return std::string(static_cast<size_t>(v[0] - '0' + 1), '#') + " ";
                }
            }
        }
        return "";
    });
    return finish(w);
}

Result<std::string> extract_xlsx(std::string_view bytes) {
    Zip zip(bytes);
    if (!zip.ok()) return make_error("malformed", "not an xlsx workbook (no zip package)");
    const std::string book_part = main_part(zip, "xl/workbook.xml");
    auto book_xml = zip.read(book_part);
    if (!book_xml) return book_xml.error();
    pugi::xml_document book;
    if (auto p = parse(book, *book_xml, book_part); !p) return p.error();
    const auto rels = relationships(zip, rels_of(book_part));

    // Shared strings: the part named by the workbook's relationship, else the usual name.
    std::vector<std::string> shared;
    std::string shared_part = "xl/sharedStrings.xml";
    {
        auto rx = zip.read(rels_of(book_part));
        pugi::xml_document rd;
        if (rx && parse(rd, *rx, rels_of(book_part))) {
            for (const auto& r : rd.document_element().children()) {
                const std::string type = attr(r, "Type");
                if (type.size() >= 14 && type.compare(type.size() - 14, 14, "/sharedStrings") == 0)
                    shared_part = resolve_part(book_part, attr(r, "Target"));
            }
        }
    }
    if (zip.has(shared_part)) {
        auto sx = zip.read(shared_part);
        if (!sx) return sx.error();
        pugi::xml_document sd;
        if (auto p = parse(sd, *sx, shared_part); !p) return p.error();
        for (const auto& si : sd.document_element().children())
            if (local(si.name()) == "si") shared.push_back(text_of(si));
    }

    Writer w;
    for (const auto& sheets : book.document_element().children()) {
        if (local(sheets.name()) != "sheets") continue;
        for (const auto& sheet : sheets.children()) {
            if (local(sheet.name()) != "sheet") continue;
            const auto rel = rels.find(rel_id(sheet));
            if (rel == rels.end()) continue;
            const std::string part = resolve_part(book_part, rel->second);
            if (!zip.has(part)) continue;  // chart sheets and dialog sheets have no cells
            auto sx = zip.read(part);
            if (!sx) return sx.error();
            pugi::xml_document sd;
            if (auto p = parse(sd, *sx, part); !p) return p.error();
            w.append((w.out.empty() ? "" : "\n") + std::string("## Sheet: ") + attr(sheet, "name") + "\n");
            for (const auto& data : sd.document_element().children()) {
                if (local(data.name()) != "sheetData") continue;
                for (const auto& row : data.children()) {
                    if (local(row.name()) != "row") continue;
                    std::vector<std::string> cells;
                    bool any = false;
                    for (const auto& c : row.children()) {
                        if (local(c.name()) != "c") continue;
                        const std::string type = attr(c, "t");
                        std::string value;
                        if (type == "inlineStr") {
                            value = text_of(c);
                        } else {
                            std::string v;
                            for (const auto& ch : c.children())
                                if (local(ch.name()) == "v") v = ch.child_value();
                            if (type == "s") {
                                char* end = nullptr;
                                const unsigned long idx = std::strtoul(v.c_str(), &end, 10);
                                value = !v.empty() && end && *end == '\0' && idx < shared.size() ? shared[idx] : std::string{};
                            } else if (type == "b") {
                                value = v == "1" ? "TRUE" : v == "0" ? "FALSE" : v;
                            } else {
                                value = v;
                            }
                        }
                        // Keep columns aligned across rows: skipped cells stay as empty cells (at most 50 in a row).
                        const long col = column_of(attr(c, "r"));
                        if (col > static_cast<long>(cells.size())) cells.resize(std::min<size_t>(static_cast<size_t>(col), cells.size() + 50));
                        cells.push_back(Writer::trim(value));
                        any = any || !cells.back().empty();
                    }
                    if (any) {
                        std::string line;
                        for (size_t i = 0; i < cells.size(); ++i) line += (i ? " | " : "") + cells[i];
                        w.append(line + "\n");
                    }
                    if (w.too_large()) return finish(w);
                }
            }
        }
    }
    if (w.out.empty() && rels.empty()) return make_error("malformed", "the workbook has no sheets");
    return finish(w);
}

Result<std::string> extract_pptx(std::string_view bytes) {
    Zip zip(bytes);
    if (!zip.ok()) return make_error("malformed", "not a pptx presentation (no zip package)");
    const std::string pres_part = main_part(zip, "ppt/presentation.xml");
    auto pres_xml = zip.read(pres_part);
    if (!pres_xml) return pres_xml.error();
    pugi::xml_document pres;
    if (auto p = parse(pres, *pres_xml, pres_part); !p) return p.error();
    const auto rels = relationships(zip, rels_of(pres_part));
    Writer w;
    int number = 0;
    for (const auto& list : pres.document_element().children()) {
        if (local(list.name()) != "sldIdLst") continue;
        for (const auto& sld : list.children()) {
            if (local(sld.name()) != "sldId") continue;
            ++number;
            const auto rel = rels.find(rel_id(sld));
            if (rel == rels.end()) continue;
            const std::string part = resolve_part(pres_part, rel->second);
            auto sx = zip.read(part);
            if (!sx) return sx.error();
            pugi::xml_document sd;
            if (auto p = parse(sd, *sx, part); !p) return p.error();
            w.append((w.out.empty() ? "" : "\n") + std::string("## Slide ") + std::to_string(number) + "\n");
            write_body(sd.document_element(), w, [](const pugi::xml_node& p) -> std::string {
                // Bulleted paragraphs (a:pPr with a:buChar or a:buAutoNum) become list items.
                for (const auto& ppr : p.children()) {
                    if (local(ppr.name()) != "pPr") continue;
                    for (const auto& c : ppr.children()) {
                        const auto l = local(c.name());
                        if (l == "buChar" || l == "buAutoNum") return "- ";
                    }
                }
                return "";
            });
            if (w.too_large()) return finish(w);
        }
    }
    return finish(w);
}

Result<std::string> extract_document(std::string_view kind, std::string_view bytes) {
    if (kind == "docx") return extract_docx(bytes);
    if (kind == "xlsx") return extract_xlsx(bytes);
    if (kind == "pptx") return extract_pptx(bytes);
#ifdef _WIN32
    if (kind == "pdf") return extract_pdf(bytes);
#endif
    return make_error("unsupported", "no text extraction for " + std::string(kind));
}

}  // namespace wb::extract
