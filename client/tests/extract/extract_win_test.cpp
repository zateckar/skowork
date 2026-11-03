// Document Text Extraction on Windows (add-core-tools-and-mcp task 1.4): PDF text through PDFium, and every document
// extracted by a sandboxed `workbench-worker.exe --role extract` process; a crashing, hanging or runaway extractor ends
// with an extraction error while the caller keeps running.
#include "extract/extract.h"
#include "extract/ooxml_fixtures.h"
#include "platform/crypto.h"
#include "platform/win.h"
#include "policy/paths.h"
#include "sandbox/proxy.h"
#include "tools/extractor.h"
#include "tools/files.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

using namespace wb;
using runtime::ToolContext;

namespace {

// A one-page PDF with one line of Helvetica text and a correct cross-reference table.
std::string tiny_pdf(const std::string& line) {
    const std::string content = "BT /F1 18 Tf 20 100 Td (" + line + ") Tj ET";
    const std::vector<std::string> objects = {
        "<< /Type /Catalog /Pages 2 0 R >>",
        "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 400 144] /Contents 4 0 R /Resources << /Font << /F1 5 0 R >> >> >>",
        "<< /Length " + std::to_string(content.size()) + " >>\nstream\n" + content + "\nendstream",
        "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
    };
    std::string pdf = "%PDF-1.4\n";
    std::vector<size_t> offsets;
    for (size_t i = 0; i < objects.size(); ++i) {
        offsets.push_back(pdf.size());
        pdf += std::to_string(i + 1) + " 0 obj\n" + objects[i] + "\nendobj\n";
    }
    const size_t xref = pdf.size();
    pdf += "xref\n0 " + std::to_string(objects.size() + 1) + "\n0000000000 65535 f \n";
    for (const size_t o : offsets) {
        char line_buf[32];
        std::snprintf(line_buf, sizeof line_buf, "%010zu 00000 n \n", o);
        pdf += line_buf;
    }
    pdf += "trailer\n<< /Size " + std::to_string(objects.size() + 1) + " /Root 1 0 R >>\nstartxref\n" + std::to_string(xref) + "\n%%EOF\n";
    return pdf;
}

std::filesystem::path exe_dir() {
    wchar_t buf[MAX_PATH];
    const DWORD n = ::GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return std::filesystem::path(std::wstring(buf, n)).parent_path();
}

struct ExtractFixture {
    sandbox::EgressProxy proxy{[](const std::string&) { return policy::RuleVerdict{false, "test", "no network in tests"}; },
                               [](const sandbox::EgressProxy::Owner&, const std::string&, const policy::RuleVerdict&) {}};
    sandbox::Sandbox box{&proxy};
    tools::SandboxExtractorOptions options;
    ToolContext ctx;

    ExtractFixture() {
        options.worker_exe = exe_dir() / L"workbench-worker.exe";
        options.sandbox = &box;
    }
    Result<std::string> run(const std::string& kind, const std::string& bytes) { return tools::sandbox_extractor(options)(kind, bytes, ctx); }
};

}  // namespace

TEST(ExtractPdf, TextOfEveryPage) {
    auto t = extract::extract_pdf(tiny_pdf("Hello PDF world"));
    ASSERT_TRUE(t) << t.error().code << ": " << t.error().message;
    EXPECT_EQ(*t, "## Page 1\nHello PDF world\n");
    auto bad = extract::extract_pdf("%PDF-1.7\nnot really");
    ASSERT_FALSE(bad);
    EXPECT_EQ(bad.error().code, "malformed");
    EXPECT_FALSE(extract::extract_pdf(""));
}

TEST(ExtractWorker, DocumentsAreExtractedInTheSandbox) {
    ExtractFixture f;
    ASSERT_TRUE(std::filesystem::exists(f.options.worker_exe));
    auto docx = f.run("docx", test::docx());
    ASSERT_TRUE(docx) << docx.error().code << ": " << docx.error().message;
    EXPECT_EQ(docx->find("# Quarterly report\n"), 0u) << *docx;
    auto xlsx = f.run("xlsx", test::xlsx());
    ASSERT_TRUE(xlsx) << xlsx.error().message;
    EXPECT_NE(xlsx->find("Alice |  | 3.5"), std::string::npos);
    auto pdf = f.run("pdf", tiny_pdf("Sandboxed PDF"));
    ASSERT_TRUE(pdf) << pdf.error().code << ": " << pdf.error().message;
    EXPECT_EQ(*pdf, "## Page 1\nSandboxed PDF\n");
    auto bad = f.run("pptx", "garbage");
    ASSERT_FALSE(bad);
    EXPECT_EQ(bad.error().code, "extract_failed");
    EXPECT_NE(bad.error().message.find("not a pptx presentation"), std::string::npos) << bad.error().message;
}

TEST(ExtractWorker, CrashHangAndRunawayParsersAreContained) {
#ifndef WORKBENCH_DEV_ENDPOINTS
    GTEST_SKIP() << "the failing-parser hooks exist in dev builds only";
#endif
    ExtractFixture f;
    auto crash = f.run("test-crash", "x");
    ASSERT_FALSE(crash);
    EXPECT_EQ(crash.error().code, "extract_failed");

    f.options.timeout = std::chrono::seconds(2);
    const auto start = std::chrono::steady_clock::now();
    auto hang = f.run("test-hang", "x");
    ASSERT_FALSE(hang);
    EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(15));
    EXPECT_NE(hang.error().message.find("did not finish within 2 seconds"), std::string::npos) << hang.error().message;

    f.options.timeout = std::chrono::seconds(30);
    f.options.memory_mb = 256;
    auto runaway = f.run("test-alloc", "x");
    ASSERT_FALSE(runaway);
    EXPECT_EQ(runaway.error().code, "resource_limit") << runaway.error().message;

    // The caller is unaffected and the next document extracts normally.
    f.options.memory_mb = 1024;
    EXPECT_TRUE(f.run("docx", test::docx()));
}

TEST(ExtractWorker, FilesReadReturnsDocumentText) {
    ExtractFixture f;
    const auto root = std::filesystem::path(local_appdata()) / L"workbench-test" / widen(random_urlsafe(6));
    const auto data = root / L"data", ws = data / L"workspace";
    std::filesystem::create_directories(ws);
    std::ofstream(ws / L"report.docx", std::ios::binary) << test::docx();
    tools::FileToolsEnv env;
    env.workspace = *policy::canonicalize(narrow(ws.wstring()));
    env.rules = [&] { return policy::path_rules(nullptr, narrow(ws.wstring()), narrow(data.wstring()), {}); };
    env.extract = tools::sandbox_extractor(f.options);
    runtime::ToolOutput out;
    for (auto& t : tools::file_tools(env))
        if (t.id == "files.read") out = t.run(f.ctx, {{"path", "report.docx"}});
    EXPECT_TRUE(out.ok) << out.content;
    EXPECT_NE(out.content.find("# Quarterly report"), std::string::npos) << out.content;
    EXPECT_NE(out.content.find("Region | Sales"), std::string::npos) << out.content;
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}
