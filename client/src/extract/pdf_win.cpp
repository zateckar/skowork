// PDF text through PDFium (add-core-tools-and-mcp design D4): pdfium.dll from bblanchon/pdfium-binaries, loaded from
// the executable's own folder only by the extract worker. The C API is declared here, so no PDFium headers are needed.
#include "extract/extract.h"

#include <windows.h>

#include <climits>
#include <string>

namespace wb::extract {

namespace {

using FPDF_DOCUMENT = void*;
using FPDF_PAGE = void*;
using FPDF_TEXTPAGE = void*;

struct Pdfium {
    void (*InitLibrary)() = nullptr;
    FPDF_DOCUMENT (*LoadMemDocument)(const void*, int, const char*) = nullptr;
    unsigned long (*GetLastError)() = nullptr;
    int (*GetPageCount)(FPDF_DOCUMENT) = nullptr;
    FPDF_PAGE (*LoadPage)(FPDF_DOCUMENT, int) = nullptr;
    FPDF_TEXTPAGE (*TextLoadPage)(FPDF_PAGE) = nullptr;
    int (*TextCountChars)(FPDF_TEXTPAGE) = nullptr;
    int (*TextGetText)(FPDF_TEXTPAGE, int, int, unsigned short*) = nullptr;
    void (*TextClosePage)(FPDF_TEXTPAGE) = nullptr;
    void (*ClosePage)(FPDF_PAGE) = nullptr;
    void (*CloseDocument)(FPDF_DOCUMENT) = nullptr;
    bool loaded = false;
};

constexpr unsigned long kErrPassword = 4;

template <typename F>
bool bind(HMODULE m, const char* name, F& f) {
    f = reinterpret_cast<F>(reinterpret_cast<void*>(::GetProcAddress(m, name)));
    return f != nullptr;
}

const Pdfium& pdfium() {
    static const Pdfium p = [] {
        Pdfium r;
        wchar_t exe[MAX_PATH];
        const DWORD n = ::GetModuleFileNameW(nullptr, exe, MAX_PATH);
        if (n == 0 || n >= MAX_PATH) return r;
        std::wstring dll(exe, n);
        dll = dll.substr(0, dll.find_last_of(L'\\') + 1) + L"pdfium.dll";
        HMODULE m = ::LoadLibraryExW(dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!m) return r;
        r.loaded = bind(m, "FPDF_InitLibrary", r.InitLibrary) && bind(m, "FPDF_LoadMemDocument", r.LoadMemDocument) &&
                   bind(m, "FPDF_GetLastError", r.GetLastError) && bind(m, "FPDF_GetPageCount", r.GetPageCount) &&
                   bind(m, "FPDF_LoadPage", r.LoadPage) && bind(m, "FPDFText_LoadPage", r.TextLoadPage) &&
                   bind(m, "FPDFText_CountChars", r.TextCountChars) && bind(m, "FPDFText_GetText", r.TextGetText) &&
                   bind(m, "FPDFText_ClosePage", r.TextClosePage) && bind(m, "FPDF_ClosePage", r.ClosePage) &&
                   bind(m, "FPDF_CloseDocument", r.CloseDocument);
        if (r.loaded) r.InitLibrary();
        return r;
    }();
    return p;
}

void append_utf8(std::string& out, const unsigned short* s, int n) {
    for (int i = 0; i < n; ++i) {
        char32_t c = s[i];
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < n && s[i + 1] >= 0xDC00 && s[i + 1] <= 0xDFFF) {
            c = 0x10000 + ((c - 0xD800) << 10) + (s[i + 1] - 0xDC00);
            ++i;
        } else if (c >= 0xD800 && c <= 0xDFFF) {
            c = 0xFFFD;
        }
        if (c == '\r') continue;  // PDFium ends lines with \r\n
        if (c == 0 || c == 0xFFFE || c == 2) continue;  // terminator, and PDFium's hyphenation marker
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
}

}  // namespace

Result<std::string> extract_pdf(std::string_view bytes) {
    const Pdfium& p = pdfium();
    if (!p.loaded) return make_error("unavailable", "PDF text extraction is not available (pdfium.dll is missing)");
    if (bytes.size() > static_cast<size_t>(INT_MAX)) return make_error("too_large", "the PDF is too large");
    FPDF_DOCUMENT doc = p.LoadMemDocument(bytes.data(), static_cast<int>(bytes.size()), nullptr);
    if (!doc) {
        if (p.GetLastError() == kErrPassword) return make_error("password", "the PDF is password-protected");
        return make_error("malformed", "not a readable PDF document");
    }
    std::string out;
    bool too_large = false;
    const int pages = p.GetPageCount(doc);
    for (int i = 0; i < pages && !too_large; ++i) {
        FPDF_PAGE page = p.LoadPage(doc, i);
        if (!page) continue;
        FPDF_TEXTPAGE text = p.TextLoadPage(page);
        std::string page_text;
        if (text) {
            const int count = p.TextCountChars(text);
            if (count > 0) {
                std::basic_string<unsigned short> buf(static_cast<size_t>(count) + 1, 0);
                const int got = p.TextGetText(text, 0, count, buf.data());
                append_utf8(page_text, buf.data(), got > 0 ? got : 0);
            }
            p.TextClosePage(text);
        }
        p.ClosePage(page);
        out += (out.empty() ? "" : "\n") + std::string("## Page ") + std::to_string(i + 1) + "\n";
        while (!page_text.empty() && (page_text.back() == '\n' || page_text.back() == ' ')) page_text.pop_back();
        if (!page_text.empty()) out += page_text + "\n";
        too_large = out.size() > kMaxTextBytes;
    }
    p.CloseDocument(doc);
    if (too_large) return make_error("too_large", "the extracted text is larger than 32 MB");
    return out;
}

}  // namespace wb::extract
