#include "ui/generated_surface.h"
#include "ui/markdown.h"
#include "ui/text16.h"

namespace wb::ui {
namespace { constexpr size_t kMaxBytes = 65536; }

std::vector<GeneratedSurfaceBlock> discover_generated_surfaces(std::string_view markdown) {
    std::vector<GeneratedSurfaceBlock> out;
    if (markdown.size() > 1024 * 1024) return out;
    unsigned candidates = 0;
    size_t cursor = 0;
    while (cursor < markdown.size()) {
        const size_t begin = cursor;
        size_t endline = markdown.find('\n', cursor);
        if (endline == std::string_view::npos) endline = markdown.size();
        auto line = markdown.substr(cursor, endline - cursor);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        cursor = endline < markdown.size() ? endline + 1 : endline;
        if (line.empty() || (line[0] != '`' && line[0] != '~')) continue;
        const char marker = line[0];
        size_t n = 0;
        while (n < line.size() && line[n] == marker) ++n;
        if (n < 3 || line.substr(n) != "skowork-ui") continue;
        if (++candidates > 16) return {};
        const size_t body_start = cursor;
        while (cursor < markdown.size()) {
            const size_t close_start = cursor;
            size_t close_end = markdown.find('\n', cursor);
            if (close_end == std::string_view::npos) close_end = markdown.size();
            auto close = markdown.substr(cursor, close_end - cursor);
            if (!close.empty() && close.back() == '\r') close.remove_suffix(1);
            cursor = close_end < markdown.size() ? close_end + 1 : close_end;
            size_t count = 0;
            while (count < close.size() && close[count] == marker) ++count;
            if (count < n || close.substr(count).find_first_not_of(" \t") != std::string_view::npos) continue;
            auto body = markdown.substr(body_start, close_start - body_start);
            if (body.size() > kMaxBytes) break;
            auto surface = parse_generated_surface(body);
            if (!surface) break;
            // Confirm the candidate is actually a top-level code block in the full Markdown context. The raw
            // closing-fence check above is essential: Markdown itself accepts a fence left open at end of input.
            auto blocks = parse_markdown(markdown.substr(0, cursor));
            if (!blocks.empty()) {
                const auto& block = blocks.back();
                while (!body.empty() && (body.back() == '\n' || body.back() == '\r')) body.remove_suffix(1);
                std::string normalized;
                normalized.reserve(body.size());
                for (size_t i = 0; i < body.size(); ++i) {
                    if (body[i] == '\r' && i + 1 < body.size() && body[i + 1] == '\n') continue;
                    normalized.push_back(body[i]);
                }
                if (block.kind == BlockKind::code && block.quote == 0 && block.indent == 0 &&
                    block.language == u"skowork-ui" && block.text == to_utf16(normalized))
                    out.push_back({begin, cursor, std::move(*surface)});
            }
            break;
        }
    }
    return out;
}

namespace {
// The words a surface shows, for the search index: every string except identifiers, types, routes and states.
void shown_words(const json& value, const std::string& key, std::string& out) {
    if (value.is_string()) {
        if (key == "type" || key == "id" || key == "template" || key == "link" || key == "tone" || key == "state" || key == "citations")
            return;
        out += value.get_ref<const std::string&>();
        out += '\n';
    } else if (value.is_object()) {
        for (auto it = value.begin(); it != value.end(); ++it) shown_words(*it, it.key(), out);
    } else if (value.is_array()) {
        for (const auto& v : value) shown_words(v, key, out);
    }
}

std::string text_of(std::string_view markdown, bool all_words) {
    if (markdown.find("skowork-ui") == std::string_view::npos) return std::string(markdown);
    const auto blocks = discover_generated_surfaces(markdown);
    const auto next_line = [&](size_t from) {
        const size_t e = markdown.find('\n', from);
        return e == std::string_view::npos ? markdown.size() : e + 1;
    };
    std::string out;
    size_t cursor = 0, next = 0;
    while (cursor < markdown.size()) {
        if (next < blocks.size() && blocks[next].begin == cursor) {
            if (all_words) shown_words(blocks[next].surface, {}, out);
            else out += jstr(blocks[next].surface, "summary") + "\n";
            cursor = blocks[next++].end;
            continue;
        }
        const size_t endline = next_line(cursor);
        const auto line = markdown.substr(cursor, endline - cursor);
        cursor = endline;
        auto bare = line;
        while (!bare.empty() && (bare.back() == '\n' || bare.back() == '\r' || bare.back() == ' ' || bare.back() == '\t'))
            bare.remove_suffix(1);
        size_t n = 0;
        if (!bare.empty() && (bare[0] == '`' || bare[0] == '~'))
            while (n < bare.size() && bare[n] == bare[0]) ++n;
        if (n < 3 || bare.substr(n) != "skowork-ui") {
            out += line;
            continue;
        }
        // An invalid or incomplete surface: left out up to its closing fence or the end of the message.
        const char marker = bare[0];
        while (cursor < markdown.size()) {
            const size_t close_end = next_line(cursor);
            const auto close = markdown.substr(cursor, close_end - cursor);
            cursor = close_end;
            size_t count = 0;
            while (count < close.size() && close[count] == marker) ++count;
            if (count >= n && close.substr(count).find_first_not_of(" \t\r\n") == std::string_view::npos) break;
        }
    }
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' ')) out.pop_back();
    return out;
}
}  // namespace

std::string generated_surface_text(std::string_view markdown) { return text_of(markdown, false); }
std::string generated_surface_search_text(std::string_view markdown) { return text_of(markdown, true); }
}  // namespace wb::ui
