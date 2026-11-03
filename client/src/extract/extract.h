// Document Text Extraction of client/core-tools (add-core-tools-and-mcp design D4). Runs only inside the sandboxed
// `workbench-worker.exe --role extract` process: every function here parses untrusted input. The OOXML extractors are
// portable (miniz for the zip container, pugixml for the parts) and fuzzed; PDF goes through PDFium on Windows.
#pragma once

#include "base/result.h"

#include <string>
#include <string_view>

namespace wb::extract {

// Limits against zip bombs and runaway output.
inline constexpr size_t kMaxEntries = 20000;
inline constexpr size_t kMaxPartBytes = 64u << 20;    // one uncompressed part
inline constexpr size_t kMaxTotalBytes = 256u << 20;  // all parts read from one document
inline constexpr size_t kMaxTextBytes = 32u << 20;    // extracted text

// Text with plain-text structure: "#" headings and "- " list items (docx), "## Sheet: name" and rows of cells joined by
// " | " (xlsx), "## Slide n" (pptx). Errors: "malformed" (not a readable document of that kind), "too_large".
Result<std::string> extract_docx(std::string_view bytes);
Result<std::string> extract_xlsx(std::string_view bytes);
Result<std::string> extract_pptx(std::string_view bytes);

// "docx", "xlsx", "pptx" here; "pdf" through extract_pdf (Windows). Error "unsupported" for other kinds.
Result<std::string> extract_document(std::string_view kind, std::string_view bytes);

#ifdef _WIN32
// PDFium from pdfium.dll next to the executable, loaded on first use. Errors: "unavailable" (no pdfium.dll),
// "malformed", "password" (encrypted), "too_large".
Result<std::string> extract_pdf(std::string_view bytes);
#endif

}  // namespace wb::extract
