// Document Text Extraction, portable part (add-core-tools-and-mcp task 1.4): docx, xlsx and pptx text with structure,
// and malformed, oversized and deeply nested documents.
#include "extract/extract.h"
#include "extract/ooxml_fixtures.h"

#include <gtest/gtest.h>

using namespace wb;
using namespace wb::extract;
using wb::test::zip_of;

TEST(ExtractOoxml, DocxKeepsHeadingsListsAndTables) {
    auto t = extract_docx(test::docx());
    ASSERT_TRUE(t) << t.error().message;
    EXPECT_EQ(*t,
              "# Quarterly report\n\n## Results\nRevenue grew 12 %\n- First point\nRegion | Sales\n\xC4\x8C" "esko | 42\n\nafter\ttab\n");
    // Without the package relationships, the usual part name is used.
    auto bare = extract_docx(zip_of({{"word/document.xml", "<w:document xmlns:w='x'><w:body><w:p><w:r><w:t>only</w:t></w:r></w:p></w:body></w:document>"}}));
    ASSERT_TRUE(bare) << bare.error().message;
    EXPECT_EQ(*bare, "only\n");
}

TEST(ExtractOoxml, XlsxSheetsRowsAndSharedStrings) {
    auto t = extract_xlsx(test::xlsx());
    ASSERT_TRUE(t) << t.error().message;
    EXPECT_EQ(*t, "## Sheet: Data\nName | Amount\nAlice |  | 3.5\nx | TRUE | \n\n## Sheet: Empty\n");
}

TEST(ExtractOoxml, PptxSlidesInPresentationOrder) {
    auto t = extract_pptx(test::pptx());
    ASSERT_TRUE(t) << t.error().message;
    EXPECT_EQ(*t, "## Slide 1\nWelcome\n- Point\n\n## Slide 2\nA | B\n");
    EXPECT_TRUE(extract_document("pptx", test::pptx()));
    EXPECT_EQ(extract_document("odt", "x").error().code, "unsupported");
}

TEST(ExtractOoxml, MalformedDocumentsFailCleanly) {
    for (const std::string& bad : {std::string(), std::string("not a zip"), std::string("PK\x03\x04garbage", 11)}) {
        for (auto* f : {&extract_docx, &extract_xlsx, &extract_pptx}) {
            auto r = (*f)(bad);
            ASSERT_FALSE(r);
            EXPECT_EQ(r.error().code, "malformed");
        }
    }
    auto broken = extract_docx(zip_of({{"word/document.xml", "<w:document><w:body><w:p>"}}));
    ASSERT_FALSE(broken);
    EXPECT_EQ(broken.error().code, "malformed");
    EXPECT_NE(broken.error().message.find("not well-formed"), std::string::npos);
    auto missing = extract_xlsx(zip_of({{"other.xml", "<x/>"}}));
    ASSERT_FALSE(missing);
    EXPECT_EQ(missing.error().code, "malformed");
    // A relationship pointing outside the package, or to a missing part.
    auto escape = extract_pptx(zip_of({{"ppt/presentation.xml", "<p:presentation xmlns:p='p' xmlns:r='r'><p:sldIdLst><p:sldId r:id='rId1'/></p:sldIdLst></p:presentation>"},
                                       {"ppt/_rels/presentation.xml.rels", "<Relationships><Relationship Id='rId1' Target='../../../../etc/passwd'/></Relationships>"}}));
    ASSERT_FALSE(escape);
    EXPECT_EQ(escape.error().code, "malformed");
}

TEST(ExtractOoxml, ZipBombsAndDeepNestingAreBounded) {
    // 80 MB of zeros compress to a few hundred KB; the part limit is 64 MB.
    const std::string zeros(80u << 20, '\0');
    auto bomb = extract_docx(zip_of({{"word/document.xml", zeros}}));
    ASSERT_FALSE(bomb);
    EXPECT_EQ(bomb.error().code, "too_large");

    std::string deep = "<w:document xmlns:w='x'><w:body>";
    for (int i = 0; i < 200000; ++i) deep += "<w:p>";
    deep += "<w:t>inside</w:t>";
    for (int i = 0; i < 200000; ++i) deep += "</w:p>";
    deep += "</w:body></w:document>";
    auto nested = extract_docx(zip_of({{"word/document.xml", deep}}));
    ASSERT_TRUE(nested) << nested.error().message;
    EXPECT_NE(nested->find("inside"), std::string::npos);
}
