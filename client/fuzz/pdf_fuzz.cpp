// Fuzz target for the PDF path of Document Text Extraction (add-core-tools-and-mcp task 1.4): the PDFium loader and the
// text conversion around it, with pdfium.dll next to the executable. PDFium itself is fuzzed upstream (OSS-Fuzz); this
// target checks our use of its API: any input must give text or an error, never a crash or unbounded output.
#include "extract/extract.h"
#include "fuzz_main.h"

using namespace wb::extract;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    auto r = extract_pdf(std::string_view(reinterpret_cast<const char*>(data), size));
    if (!r && r.error().code == "unavailable") __builtin_trap();  // the smoke run must exercise PDFium
    if (r && r->size() > kMaxTextBytes + (1u << 20)) __builtin_trap();
    return 0;
}
