// Worker request handlers. Each role registers the jobs it runs; later changes add extraction, indexing and
// transcription here.
#pragma once

#include "base/json.h"
#include "base/result.h"

#include <string>

namespace wb {

inline std::string narrow_ascii(const wchar_t* w) {
    std::string s;
    for (; w && *w; ++w) s.push_back(*w < 0x80 ? static_cast<char>(*w) : '?');
    return s;
}

namespace worker {

Result<json> dispatch(const std::string& role, const std::string& method, const json& params);

// `--role extract --kind <docx|xlsx|pptx|pdf>`: reads the document from `in` until EOF, writes its text to `out` and
// returns 0; on failure writes "<code>: <message>" to `err` and returns 1.
int extract_once(const std::string& kind, void* in, void* out, void* err);

}  // namespace worker
}  // namespace wb
