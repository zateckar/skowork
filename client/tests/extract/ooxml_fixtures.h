// Small OOXML packages built in memory for the extraction tests (add-core-tools-and-mcp task 1.4).
#pragma once

#include <miniz.h>

#include <string>
#include <utility>
#include <vector>

namespace wb::test {

using Parts = std::vector<std::pair<std::string, std::string>>;

inline std::string zip_of(const Parts& parts) {
    mz_zip_archive zip{};
    mz_zip_writer_init_heap(&zip, 0, 0);
    for (const auto& [name, content] : parts)
        mz_zip_writer_add_mem(&zip, name.c_str(), content.data(), content.size(), MZ_DEFAULT_COMPRESSION);
    void* buf = nullptr;
    size_t size = 0;
    mz_zip_writer_finalize_heap_archive(&zip, &buf, &size);
    std::string out(static_cast<const char*>(buf), size);
    mz_zip_writer_end(&zip);
    mz_free(buf);
    return out;
}

inline std::string rels(const std::string& target) {
    return R"(<?xml version="1.0" encoding="UTF-8"?><Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">)"
           R"(<Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target=")" +
           target + R"("/></Relationships>)";
}

inline std::string docx() {
    const std::string doc = R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<w:document xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main"><w:body>
<w:p><w:pPr><w:pStyle w:val="Title"/></w:pPr><w:r><w:t>Quarterly report</w:t></w:r></w:p>
<w:p><w:pPr><w:pStyle w:val="Heading2"/></w:pPr><w:r><w:t>Results</w:t></w:r></w:p>
<w:p><w:r><w:t xml:space="preserve">Revenue grew </w:t></w:r><w:r><w:rPr><w:b/></w:rPr><w:t>12 %</w:t></w:r><w:del><w:r><w:delText>deleted</w:delText></w:r></w:del></w:p>
<w:p><w:pPr><w:numPr><w:ilvl w:val="0"/><w:numId w:val="1"/></w:numPr></w:pPr><w:r><w:t>First point</w:t></w:r></w:p>
<w:tbl><w:tblPr/><w:tr><w:tc><w:p><w:r><w:t>Region</w:t></w:r></w:p></w:tc><w:tc><w:p><w:r><w:t>Sales</w:t></w:r></w:p></w:tc></w:tr>
<w:tr><w:tc><w:p><w:r><w:t>Česko</w:t></w:r></w:p></w:tc><w:tc><w:p><w:r><w:t>42</w:t></w:r></w:p></w:tc></w:tr></w:tbl>
<w:p><w:r><w:instrText>HYPERLINK "x"</w:instrText></w:r><w:r><w:t>after</w:t><w:tab/><w:t>tab</w:t></w:r></w:p>
<w:sectPr/></w:body></w:document>)";
    return zip_of({{"[Content_Types].xml", "<Types/>"}, {"_rels/.rels", rels("word/document.xml")}, {"word/document.xml", doc}});
}

inline std::string xlsx() {
    const std::string book = R"(<?xml version="1.0" encoding="UTF-8"?><workbook xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main" xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships"><sheets><sheet name="Data" sheetId="1" r:id="rId1"/><sheet name="Empty" sheetId="2" r:id="rId3"/></sheets></workbook>)";
    const std::string book_rels = R"(<?xml version="1.0" encoding="UTF-8"?><Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"><Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet" Target="worksheets/sheet1.xml"/><Relationship Id="rId2" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/sharedStrings" Target="sharedStrings.xml"/><Relationship Id="rId3" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet" Target="/xl/worksheets/sheet2.xml"/></Relationships>)";
    const std::string shared = R"(<?xml version="1.0" encoding="UTF-8"?><sst xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main"><si><t>Name</t></si><si><t>Amount</t></si><si><r><t>Al</t></r><r><t>ice</t></r><rPh><t>phonetic</t></rPh></si></sst>)";
    const std::string sheet = R"(<?xml version="1.0" encoding="UTF-8"?><worksheet xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main"><sheetData>)"
                              R"(<row r="1"><c r="A1" t="s"><v>0</v></c><c r="B1" t="s"><v>1</v></c></row>)"
                              R"(<row r="2"><c r="A2" t="s"><v>2</v></c><c r="C2"><v>3.5</v></c></row>)"
                              R"(<row r="3"><c r="A3" t="inlineStr"><is><t>x</t></is></c><c r="B3" t="b"><v>1</v></c><c r="C3" t="s"><v>99</v></c></row>)"
                              R"(</sheetData></worksheet>)";
    const std::string sheet2 = R"(<?xml version="1.0" encoding="UTF-8"?><worksheet xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main"><sheetData/></worksheet>)";
    return zip_of({{"_rels/.rels", rels("xl/workbook.xml")},
                   {"xl/workbook.xml", book},
                   {"xl/_rels/workbook.xml.rels", book_rels},
                   {"xl/sharedStrings.xml", shared},
                   {"xl/worksheets/sheet1.xml", sheet},
                   {"xl/worksheets/sheet2.xml", sheet2}});
}

inline std::string pptx() {
    const std::string pres = R"(<?xml version="1.0" encoding="UTF-8"?><p:presentation xmlns:p="http://schemas.openxmlformats.org/presentationml/2006/main" xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships"><p:sldIdLst><p:sldId id="256" r:id="rId2"/><p:sldId id="257" r:id="rId3"/></p:sldIdLst></p:presentation>)";
    const std::string pres_rels = R"(<?xml version="1.0" encoding="UTF-8"?><Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"><Relationship Id="rId3" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/slide" Target="slides/slide2.xml"/><Relationship Id="rId2" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/slide" Target="slides/slide1.xml"/></Relationships>)";
    const std::string s1 = R"(<?xml version="1.0" encoding="UTF-8"?><p:sld xmlns:p="http://schemas.openxmlformats.org/presentationml/2006/main" xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main"><p:cSld><p:spTree><p:sp><p:txBody><a:p><a:r><a:t>Welcome</a:t></a:r></a:p><a:p><a:pPr><a:buChar char="•"/></a:pPr><a:r><a:t>Point</a:t></a:r></a:p></p:txBody></p:sp></p:spTree></p:cSld></p:sld>)";
    const std::string s2 = R"(<?xml version="1.0" encoding="UTF-8"?><p:sld xmlns:p="http://schemas.openxmlformats.org/presentationml/2006/main" xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main"><p:cSld><p:spTree><p:graphicFrame><a:graphic><a:graphicData><a:tbl><a:tr><a:tc><a:txBody><a:p><a:r><a:t>A</a:t></a:r></a:p></a:txBody></a:tc><a:tc><a:txBody><a:p><a:r><a:t>B</a:t></a:r></a:p></a:txBody></a:tc></a:tr></a:tbl></a:graphicData></a:graphic></p:graphicFrame></p:spTree></p:cSld></p:sld>)";
    return zip_of({{"_rels/.rels", rels("ppt/presentation.xml")},
                   {"ppt/presentation.xml", pres},
                   {"ppt/_rels/presentation.xml.rels", pres_rels},
                   {"ppt/slides/slide1.xml", s1},
                   {"ppt/slides/slide2.xml", s2}});
}

}  // namespace wb::test
