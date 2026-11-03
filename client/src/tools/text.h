// Portable text helpers of client/core-tools: binary detection, decoding to UTF-8, line ranges with numbers,
// truncation at a line boundary and exact-span matching. No Win32 headers, so the fuzz job builds them too.
#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace wb::tools {

// The text encodings files.read and files.edit understand. The original encoding is kept on write.
enum class TextEncoding { utf8, utf8_bom, utf16le, utf16be, ansi };

struct DecodedText {
    std::string text;  // UTF-8
    TextEncoding encoding = TextEncoding::utf8;
};

// Binary content: a NUL byte (outside UTF-16 text) or many control characters in the first 8 KB.
bool looks_binary(std::string_view bytes);

// UTF-8 (with or without BOM), UTF-16 with a BOM, otherwise Windows-1252. nullopt for binary content.
std::optional<DecodedText> decode_text(std::string_view bytes);
// Back to the original encoding (Windows-1252: characters outside it become '?').
std::string encode_text(std::string_view utf8, TextEncoding encoding);

// Document types handled by Document Text Extraction, by file extension: "docx", "xlsx", "pptx", "pdf" or "".
std::string document_kind(std::string_view path);

size_t count_lines(std::string_view text);

struct LineSlice {
    std::string text;
    size_t first = 1, last = 0;  // 1-based line numbers of what was returned (last < first: nothing)
    bool truncated = false;      // cut by the byte limit before `limit` lines
};
// Lines [first, first+limit) of `text` (limit 0: to the end), at most max_bytes in total, cut at a line boundary
// (a single over-long line is cut inside). With `numbered`, each line starts with its number and a tab.
LineSlice slice_lines(std::string_view text, size_t first, size_t limit, size_t max_bytes, bool numbered);

// Non-overlapping occurrences of `needle` in `haystack` (0 for an empty needle).
size_t count_occurrences(std::string_view haystack, std::string_view needle);

}  // namespace wb::tools
