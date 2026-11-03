// Markdown of model output (client/work-inbox Markdown Rendering; add-native-ui design D4): md4c with the GitHub
// dialect parses into blocks with styled spans, in UTF-16 code units for DirectWrite. Model output is untrusted:
//   - raw HTML stays literal text (MD_FLAG_NOHTML);
//   - an image becomes its alternative text followed by " [<url>]" as a link span; nothing is ever fetched;
//   - a link is only a span with its target; opening it is the user's click (the window asks first).
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace wb::ui {

enum class BlockKind { paragraph, heading, code, item, rule, table };

enum Style : unsigned { kBold = 1, kItalic = 2, kCode = 4, kStrike = 8, kLink = 16 };

struct Span {
    uint32_t start = 0;  // UTF-16 code units
    uint32_t length = 0;
    unsigned style = 0;
    std::u16string url;  // kLink
};

struct Cell {
    std::u16string text;
    std::vector<Span> spans;
    bool header = false;
};

struct Block {
    BlockKind kind = BlockKind::paragraph;
    int level = 0;   // heading level 1-6
    int indent = 0;  // list depth
    int quote = 0;   // block-quote depth
    std::u16string marker;  // list item: "•", "3.", "☐" or "☑"
    std::u16string language;  // code block info string
    std::u16string text;
    std::vector<Span> spans;
    std::vector<std::vector<Cell>> rows;  // table
};

std::vector<Block> parse_markdown(std::string_view utf8);

// The plain text of the blocks, one block per line, for copying and for screen readers.
std::u16string plain_text(const std::vector<Block>& blocks);

}  // namespace wb::ui
