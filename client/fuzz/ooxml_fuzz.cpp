// Fuzz target for the docx, xlsx and pptx extractors (client/core-tools Document Text Extraction; add-core-tools-and-mcp
// task 1.4). The first byte picks the format and the mode: the rest is either a whole package (zip container and part
// lookup) or XML packed into a fresh package as the format's content parts, so that mutations reach the XML walkers
// past the zip CRC checks. Any input must give text or an error, never a crash, hang or unbounded memory use.
#include "extract/extract.h"
#include "fuzz_main.h"

#include <miniz.h>

#include <string>
#include <utility>
#include <vector>

using namespace wb::extract;

namespace {

std::string package(const std::vector<std::pair<const char*, std::string_view>>& parts) {
    mz_zip_archive zip{};
    mz_zip_writer_init_heap(&zip, 0, 0);
    for (const auto& [name, content] : parts) mz_zip_writer_add_mem(&zip, name, content.data(), content.size(), MZ_NO_COMPRESSION);
    void* buf = nullptr;
    size_t size = 0;
    mz_zip_writer_finalize_heap_archive(&zip, &buf, &size);
    std::string out(static_cast<const char*>(buf), size);
    mz_zip_writer_end(&zip);
    mz_free(buf);
    return out;
}

void check(const wb::Result<std::string>& r) {
    if (r && r->size() > kMaxTextBytes + 64) __builtin_trap();
}

constexpr std::string_view kBook = R"(<workbook xmlns:r="r"><sheets><sheet name="S" r:id="rId1"/></sheets></workbook>)";
constexpr std::string_view kBookRels =
    R"(<Relationships><Relationship Id="rId1" Target="worksheets/sheet1.xml"/><Relationship Id="rId2" Type="x/sharedStrings" Target="sharedStrings.xml"/></Relationships>)";
constexpr std::string_view kPres = R"(<p:presentation xmlns:p="p" xmlns:r="r"><p:sldIdLst><p:sldId id="1" r:id="rId1"/></p:sldIdLst></p:presentation>)";
constexpr std::string_view kPresRels = R"(<Relationships><Relationship Id="rId1" Target="slides/slide1.xml"/></Relationships>)";

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size == 0) return 0;
    const std::string_view body(reinterpret_cast<const char*>(data) + 1, size - 1);
    switch (data[0] % 6) {
        case 0: check(extract_docx(body)); break;
        case 1: check(extract_xlsx(body)); break;
        case 2: check(extract_pptx(body)); break;
        case 3: check(extract_docx(package({{"word/document.xml", body}}))); break;
        case 4:
            check(extract_xlsx(package({{"xl/workbook.xml", kBook},
                                        {"xl/_rels/workbook.xml.rels", kBookRels},
                                        {"xl/sharedStrings.xml", body},
                                        {"xl/worksheets/sheet1.xml", body}})));
            break;
        default:
            check(extract_pptx(package({{"ppt/presentation.xml", kPres}, {"ppt/_rels/presentation.xml.rels", kPresRels}, {"ppt/slides/slide1.xml", body}})));
            break;
    }
    return 0;
}
