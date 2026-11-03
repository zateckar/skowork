// The in-house HTML-to-text extractor of client/core-tools (add-core-tools-and-mcp design D4): a tokenizer over tags,
// entities and text, without a DOM. It drops scripts, styles, templates and hidden elements, and keeps the structure
// as Markdown-like plain text (headings, list items, table rows, links, preformatted blocks). Portable and fuzzed.
#pragma once

#include <string>
#include <string_view>

namespace wb::tools {

struct HtmlText {
    std::string title;
    std::string text;
};

// `base_url` resolves relative link targets (may be empty). Input of any size and shape is accepted; the work is linear
// in the input, and nesting depth is bounded.
HtmlText html_to_text(std::string_view html, std::string_view base_url = {});

// Decodes character references (&amp; &#233; &#xE9; and the common named ones) in text.
std::string decode_entities(std::string_view s);

}  // namespace wb::tools
