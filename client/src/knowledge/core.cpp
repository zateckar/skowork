#include "knowledge/core.h"

#include "base/strings.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace wb::knowledge {
namespace {

bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }

bool is_continuation(unsigned char c) { return (c & 0xC0) == 0x80; }

// The largest position <= pos that does not split a UTF-8 sequence.
size_t boundary_at_or_before(std::string_view s, size_t pos) {
    if (pos >= s.size()) return s.size();
    while (pos > 0 && is_continuation(static_cast<unsigned char>(s[pos]))) --pos;
    return pos;
}

void append_utf8(std::string& out, unsigned cp) {
    if (cp < 0x80) out.push_back(static_cast<char>(cp));
    else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

constexpr unsigned kReplacement = 0xFFFD;

// Valid UTF-8, with invalid sequences (overlong, surrogates, truncated) replaced.
std::string sanitize_utf8(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        const auto c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) {
            if (c != 0) out.push_back(static_cast<char>(c));
            ++i;
            continue;
        }
        int len = 0;
        unsigned cp = 0, min = 0;
        if ((c & 0xE0) == 0xC0) len = 2, cp = c & 0x1F, min = 0x80;
        else if ((c & 0xF0) == 0xE0) len = 3, cp = c & 0x0F, min = 0x800;
        else if ((c & 0xF8) == 0xF0) len = 4, cp = c & 0x07, min = 0x10000;
        bool ok = len > 0 && i + static_cast<size_t>(len) <= s.size();
        for (int k = 1; ok && k < len; ++k) {
            const auto d = static_cast<unsigned char>(s[i + static_cast<size_t>(k)]);
            if (!is_continuation(d)) ok = false;
            else cp = (cp << 6) | (d & 0x3F);
        }
        if (ok && (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))) ok = false;
        if (!ok) {
            append_utf8(out, kReplacement);
            ++i;
            continue;
        }
        out.append(s.substr(i, static_cast<size_t>(len)));
        i += static_cast<size_t>(len);
    }
    return out;
}

std::string from_utf16(std::string_view s, bool big_endian) {
    std::string out;
    out.reserve(s.size());
    const auto unit = [&](size_t i) -> unsigned {
        const auto a = static_cast<unsigned char>(s[i]), b = static_cast<unsigned char>(s[i + 1]);
        return big_endian ? (a << 8) | b : (b << 8) | a;
    };
    for (size_t i = 0; i + 1 < s.size(); i += 2) {
        unsigned u = unit(i);
        if (u >= 0xD800 && u <= 0xDBFF && i + 3 < s.size()) {
            const unsigned lo = unit(i + 2);
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                append_utf8(out, 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00));
                i += 2;
                continue;
            }
        }
        if (u >= 0xD800 && u <= 0xDFFF) u = kReplacement;
        if (u != 0) append_utf8(out, u);
    }
    return out;
}

std::string normalise_newlines(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\r') {
            out.push_back('\n');
            if (i + 1 < s.size() && s[i + 1] == '\n') ++i;
        } else out.push_back(s[i]);
    }
    return out;
}

std::vector<std::string_view> lines_of(std::string_view s) {
    std::vector<std::string_view> out;
    size_t start = 0;
    while (start <= s.size()) {
        const size_t nl = s.find('\n', start);
        if (nl == std::string_view::npos) {
            if (start < s.size()) out.push_back(s.substr(start));
            break;
        }
        out.push_back(s.substr(start, nl - start));
        start = nl + 1;
    }
    return out;
}

bool blank(std::string_view s) { return std::all_of(s.begin(), s.end(), is_space); }

std::string trimmed(std::string_view s) { return trim(s); }

// Where a window of at most `max` bytes starting at `start` should end: after the last whitespace in its second half,
// else at a code-point boundary.
size_t cut_point(std::string_view s, size_t start, size_t max) {
    if (s.size() - start <= max) return s.size();
    const size_t limit = start + max;
    for (size_t p = limit; p > start + max / 2; --p)
        if (is_space(s[p - 1])) return p;
    const size_t b = boundary_at_or_before(s, limit);
    return b > start ? b : std::min(s.size(), start + max);
}

// The tail of a chunk carried into the next one: about kOverlapChars, starting at a word.
std::string overlap_tail(std::string_view s) {
    if (s.size() <= kOverlapChars) return std::string(s);
    size_t p = s.size() - kOverlapChars;
    while (p < s.size() && !is_space(s[p])) ++p;
    if (p >= s.size()) p = boundary_at_or_before(s, s.size() - kOverlapChars);
    return trimmed(s.substr(p));
}

// Splits text longer than kChunkChars into overlapping windows.
std::vector<std::string> windows(std::string_view s) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start < s.size()) {
        const size_t end = cut_point(s, start, kChunkChars);
        std::string piece = trimmed(s.substr(start, end - start));
        if (!piece.empty()) out.push_back(std::move(piece));
        if (end >= s.size()) break;
        // Step back by the overlap to a word start, but always move forward.
        size_t next = end - start > 2 * kOverlapChars ? end - kOverlapChars : end;
        while (next < end && !is_space(s[next])) ++next;
        next = boundary_at_or_before(s, next);
        start = next > start ? next : end;
    }
    return out;
}

// Packs units (paragraphs, symbols, messages) of one section into chunks: units are joined while they fit, the tail of
// a full chunk is carried into the next, and a unit too large on its own is split into windows.
class Packer {
public:
    explicit Packer(std::vector<Chunk>& out) : out_(out) {}

    void section(std::string heading) {
        finish();
        heading_ = std::move(heading);
    }
    void unit(std::string_view text, long long at = 0, std::string_view sep = "\n\n") {
        const std::string t = trimmed(text);
        if (t.empty()) return;
        if (t.size() > kChunkChars) {
            finish();
            for (auto& w : windows(t)) out_.push_back({heading_, std::move(w), at});
            return;
        }
        if (!cur_.empty() && cur_.size() + sep.size() + t.size() > kChunkChars) {
            if (has_new_) {
                out_.push_back({heading_, cur_, at_});
                cur_ = overlap_tail(cur_);
                has_new_ = false;
            }
            if (cur_.size() + sep.size() + t.size() > kChunkChars) cur_.clear();
        }
        if (!has_new_) at_ = at;
        if (!cur_.empty()) cur_ += sep;
        cur_ += t;
        has_new_ = true;
    }
    void finish() {
        if (has_new_) out_.push_back({heading_, cur_, at_});
        cur_.clear();
        has_new_ = false;
    }

private:
    std::vector<Chunk>& out_;
    std::string heading_, cur_;
    long long at_ = 0;
    bool has_new_ = false;  // cur_ holds more than the overlap carried from the previous chunk
};

std::string bounded(std::string s, size_t max) {
    s = trim(s);
    if (s.size() > max) s = utf8_truncate(s, max);
    return s;
}

}  // namespace

long long estimate_tokens(std::string_view text) { return static_cast<long long>((text.size() + 3) / 4); }

// ---- Files ------------------------------------------------------------------------------------------------------------

std::string file_kind(std::string_view path) {
    const size_t slash = path.find_last_of("/\\");
    const std::string_view name = slash == std::string_view::npos ? path : path.substr(slash + 1);
    const size_t dot = name.rfind('.');
    if (dot == std::string_view::npos || dot == 0) return {};
    const std::string ext = to_lower(name.substr(dot + 1));
    if (ext == "docx" || ext == "xlsx" || ext == "pptx" || ext == "pdf" || ext == "md" || ext == "txt") return ext;
    if (ext == "markdown") return "md";
    static const char* const code[] = {"c",  "h",   "cc",  "cpp", "cxx", "hpp", "hh",   "cs",  "java", "kt",  "kts", "py",
                                       "js", "jsx", "mjs", "cjs", "ts",  "tsx", "go",   "rs",  "rb",   "php", "swift",
                                       "sql", "sh", "ps1", "psm1", "bat", "cmd", "yaml", "yml", "toml", "ini", "json",
                                       "xml", "html", "htm", "css", "scss", "vue", "cmake", "gradle", "m", "r", "scala"};
    for (const char* c : code)
        if (ext == c) return "code";
    return {};
}

bool is_document_kind(std::string_view kind) { return kind == "docx" || kind == "xlsx" || kind == "pptx" || kind == "pdf"; }

bool looks_binary(std::string_view bytes) {
    if (bytes.size() >= 2 && ((bytes[0] == '\xFF' && bytes[1] == '\xFE') || (bytes[0] == '\xFE' && bytes[1] == '\xFF')))
        return false;
    return bytes.substr(0, 8192).find('\0') != std::string_view::npos;
}

std::string decode_text(std::string_view bytes) {
    if (bytes.size() >= 2 && bytes[0] == '\xFF' && bytes[1] == '\xFE') return from_utf16(bytes.substr(2), false);
    if (bytes.size() >= 2 && bytes[0] == '\xFE' && bytes[1] == '\xFF') return from_utf16(bytes.substr(2), true);
    if (bytes.size() >= 3 && bytes.substr(0, 3) == "\xEF\xBB\xBF") bytes.remove_prefix(3);
    return sanitize_utf8(bytes);
}

// ---- Chunkers -----------------------------------------------------------------------------------------------------------

std::vector<Chunk> chunk_document(std::string_view input) {
    const std::string text = normalise_newlines(input.substr(0, kMaxItemText));
    std::vector<Chunk> out;
    Packer p(out);
    std::string para;
    const auto end_para = [&] {
        p.unit(para);
        para.clear();
    };
    for (std::string_view line : lines_of(text)) {
        const std::string t = trimmed(line);
        size_t hashes = 0;
        while (hashes < t.size() && t[hashes] == '#') ++hashes;
        if (hashes >= 1 && hashes <= 6 && hashes < t.size() && t[hashes] == ' ') {
            end_para();
            p.section(bounded(t.substr(hashes + 1), 200));
            continue;
        }
        if (t.empty()) {
            end_para();
            continue;
        }
        if (!para.empty()) para.push_back('\n');
        para += line;
    }
    end_para();
    p.finish();
    return out;
}

std::vector<Chunk> chunk_code(std::string_view input) {
    const std::string text = normalise_newlines(input.substr(0, kMaxItemText));
    std::vector<Chunk> out;
    std::string unit, first;
    bool prev_blank = true, prev_closer = false;
    const auto flush = [&] {
        const std::string t = trimmed(unit);
        if (!t.empty()) {
            Packer p(out);
            p.section(bounded(first, 120));
            p.unit(t, 0, "\n");
            p.finish();
        }
        unit.clear();
        first.clear();
    };
    // Small symbols are merged with the next one so a file of one-liners does not become hundreds of chunks.
    for (std::string_view line : lines_of(text)) {
        const bool is_blank = blank(line);
        const char c0 = line.empty() ? ' ' : line[0];
        const bool closer = c0 == '}' || c0 == ')' || c0 == ']';
        const bool top = !is_blank && !is_space(c0) && !closer;
        if (top && (prev_blank || prev_closer) && unit.size() >= kChunkChars / 2) flush();
        if (first.empty() && top) first = trimmed(line);
        unit += line;
        unit.push_back('\n');
        prev_blank = is_blank;
        prev_closer = closer;
    }
    flush();
    return out;
}

std::string strip_quoted(std::string_view input) {
    const std::string text = normalise_newlines(input.substr(0, kMaxItemText));
    const auto lines = lines_of(text);
    std::string out;
    const auto starts_ci = [](std::string_view s, std::string_view p) { return s.size() >= p.size() && iequals(s.substr(0, p.size()), p); };
    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string t = trimmed(lines[i]);
        if (starts_with(t, "-----Original Message") || starts_with(t, "-----Ursprüngliche Nachricht") ||
            starts_with(t, "-----Původní zpráva") || starts_with(t, "________________________________"))
            break;
        // Outlook headers of the quoted message: From/Von/Od followed by Sent/Gesendet/Odesláno within three lines.
        if (starts_ci(t, "From:") || starts_ci(t, "Von:") || starts_ci(t, "Od:")) {
            bool header = false;
            for (size_t k = i + 1; k < lines.size() && k <= i + 3; ++k) {
                const std::string n = trimmed(lines[k]);
                if (starts_ci(n, "Sent:") || starts_ci(n, "Gesendet:") || starts_ci(n, "Odesláno:") || starts_ci(n, "Date:") ||
                    starts_ci(n, "Datum:"))
                    header = true;
            }
            if (header) break;
        }
        // "On <date>, <name> wrote:" and its German and Czech forms.
        if ((starts_with(t, "On ") && ends_with(t, "wrote:")) || (starts_with(t, "Am ") && ends_with(t, ":") && t.find(" schrieb") != std::string::npos) ||
            (starts_with(t, "Dne ") && (ends_with(t, "napsal:") || ends_with(t, "napsala:") || ends_with(t, "napsal(a):"))))
            break;
        if (starts_with(t, ">")) continue;
        out += lines[i];
        out.push_back('\n');
    }
    return trim(out);
}

std::vector<Chunk> chunk_mail(std::string_view body) {
    const std::string text = strip_quoted(body);
    std::vector<Chunk> out;
    Packer p(out);
    std::string para;
    for (std::string_view line : lines_of(text)) {
        if (blank(line)) {
            p.unit(para);
            para.clear();
            continue;
        }
        if (!para.empty()) para.push_back('\n');
        para += line;
    }
    p.unit(para);
    p.finish();
    return out;
}

std::vector<Chunk> chunk_transcript(const std::vector<Turn>& turns) {
    std::vector<Chunk> out;
    size_t i = 0;
    while (i < turns.size()) {
        const long long start = turns[i].at;
        std::string text;
        std::vector<std::string> speakers;
        size_t j = i;
        while (j < turns.size() && (j == i || (turns[j].at - start < kTranscriptWindowMs && text.size() < kChunkChars * 2))) {
            const std::string t = trimmed(decode_text(turns[j].text));
            const std::string who = bounded(decode_text(turns[j].speaker), 80);
            if (!t.empty()) {
                if (!text.empty()) text.push_back('\n');
                text += (who.empty() ? std::string("Speaker") : who) + ": " + t;
                if (!who.empty() && std::find(speakers.begin(), speakers.end(), who) == speakers.end()) speakers.push_back(who);
            }
            ++j;
        }
        std::string heading;
        for (const auto& s : speakers) heading += (heading.empty() ? "" : ", ") + s;
        if (text.size() > kChunkChars * 2) {
            for (auto& w : windows(text)) out.push_back({heading, std::move(w), start});
        } else if (!text.empty()) out.push_back({heading, std::move(text), start});
        i = j;
    }
    return out;
}

std::vector<Chunk> chunk_messages(const std::vector<Turn>& messages) {
    std::vector<Chunk> out;
    Packer p(out);
    for (const auto& m : messages) {
        const std::string t = trimmed(decode_text(m.text));
        if (t.empty()) continue;
        p.unit(bounded(decode_text(m.speaker), 80) + ": " + t, m.at);
    }
    p.finish();
    return out;
}

std::vector<Chunk> chunk_text(std::string_view kind, std::string_view text) {
    const std::string clean = decode_text(text.substr(0, kMaxItemText));
    if (kind == "code") return chunk_code(clean);
    if (kind == "mail") return chunk_mail(clean);
    return chunk_document(clean);
}

// ---- Exclusions -------------------------------------------------------------------------------------------------------

bool glob_match(std::string_view pattern, std::string_view path) {
    const auto norm = [](std::string_view s) {
        std::string o = to_lower(s.substr(0, 4096));
        std::replace(o.begin(), o.end(), '\\', '/');
        return o;
    };
    const std::string pat = norm(pattern);
    std::string p = norm(path);
    if (pat.empty()) return false;
    if (pat.find('/') == std::string::npos) {
        const size_t slash = p.rfind('/');
        if (slash != std::string::npos) p = p.substr(slash + 1);
    }
    // Tokens: 0 = **, 1 = *, 2 = ?, otherwise the character.
    std::vector<int> tok;
    for (size_t i = 0; i < pat.size(); ++i) {
        if (pat[i] == '*' && i + 1 < pat.size() && pat[i + 1] == '*') {
            tok.push_back(0);
            while (i + 1 < pat.size() && pat[i + 1] == '*') ++i;
        } else if (pat[i] == '*') tok.push_back(1);
        else if (pat[i] == '?') tok.push_back(2);
        else tok.push_back(256 + static_cast<unsigned char>(pat[i]));
    }
    // dp[j]: the pattern prefix of length i matches the path prefix of length j.
    std::vector<char> dp(p.size() + 1, 0), next(p.size() + 1, 0);
    dp[0] = 1;
    for (int t : tok) {
        std::fill(next.begin(), next.end(), 0);
        for (size_t j = 0; j <= p.size(); ++j) {
            if (t == 0 || t == 1) {
                // A star extends any earlier match across characters (not across '/' for a single star).
                if (dp[j] || (j > 0 && next[j - 1] && (t == 0 || p[j - 1] != '/'))) next[j] = 1;
            } else if (j > 0 && dp[j - 1]) {
                if (t == 2 ? p[j - 1] != '/' : p[j - 1] == static_cast<char>(t - 256)) next[j] = 1;
            }
        }
        dp.swap(next);
    }
    return dp[p.size()] != 0;
}

std::string normalize_name(std::string_view name) {
    const std::string f = fold_latin(name.substr(0, 512));
    std::string out;
    bool space = false;
    for (char c : f) {
        const auto u = static_cast<unsigned char>(c);
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || u >= 0x80 || c == '@' || c == '.' || c == '-' || c == '_') {
            // Dots and dashes count as separators except inside addresses.
            if ((c == '.' || c == '-' || c == '_') && f.find('@') == std::string::npos) {
                space = !out.empty();
                continue;
            }
            if (space) out.push_back(' ');
            space = false;
            out.push_back(c);
        } else space = !out.empty();
    }
    return out;
}

// ---- Vectors ----------------------------------------------------------------------------------------------------------

std::vector<std::uint8_t> quantize_binary(const std::vector<float>& v) {
    std::vector<std::uint8_t> out((v.size() + 7) / 8, 0);
    for (size_t i = 0; i < v.size(); ++i)
        if (v[i] > 0) out[i / 8] |= static_cast<std::uint8_t>(1u << (i % 8));
    return out;
}

std::vector<std::uint8_t> quantize_int8(const std::vector<float>& v) {
    float max = 0;
    for (float x : v)
        if (std::isfinite(x)) max = std::max(max, std::fabs(x));
    std::vector<std::uint8_t> out(v.size(), 0);
    if (max <= 0) return out;
    for (size_t i = 0; i < v.size(); ++i) {
        const float x = std::isfinite(v[i]) ? v[i] : 0.0f;
        const long q = std::lround(x / max * 127.0f);
        out[i] = static_cast<std::uint8_t>(static_cast<std::int8_t>(std::clamp<long>(q, -127, 127)));
    }
    return out;
}

int hamming(const std::uint8_t* a, const std::uint8_t* b, size_t n) {
    int d = 0;
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        std::uint64_t x = 0, y = 0;
        std::memcpy(&x, a + i, 8);
        std::memcpy(&y, b + i, 8);
        d += __builtin_popcountll(x ^ y);
    }
    for (; i < n; ++i) d += __builtin_popcount(static_cast<unsigned>(a[i] ^ b[i]));
    return d;
}

double int8_cosine(const std::uint8_t* a, const std::uint8_t* b, size_t n) {
    long long dot = 0, na = 0, nb = 0;
    for (size_t i = 0; i < n; ++i) {
        const int x = static_cast<std::int8_t>(a[i]), y = static_cast<std::int8_t>(b[i]);
        dot += x * y;
        na += x * x;
        nb += y * y;
    }
    if (na == 0 || nb == 0) return 0;
    return static_cast<double>(dot) / (std::sqrt(static_cast<double>(na)) * std::sqrt(static_cast<double>(nb)));
}

std::map<long long, double> reciprocal_rank_fusion(const std::vector<std::vector<long long>>& lists, double k) {
    std::map<long long, double> out;
    for (const auto& l : lists)
        for (size_t r = 0; r < l.size(); ++r) out[l[r]] += 1.0 / (k + static_cast<double>(r) + 1.0);
    return out;
}

// ---- Extraction -------------------------------------------------------------------------------------------------------

const std::vector<std::string>& entity_types() {
    static const std::vector<std::string> t = {"Person", "Project", "Organization", "Document", "Meeting", "Topic", "System"};
    return t;
}

const json& extraction_schema() {
    static const json s = {
        {"type", "object"},
        {"properties",
         {{"entities",
           {{"type", "array"},
            {"items",
             {{"type", "object"},
              {"properties",
               {{"name", {{"type", "string"}}},
                {"type", {{"type", "string"}, {"enum", entity_types()}}},
                {"email", {{"type", "string"}}},
                {"chunks", {{"type", "array"}, {"items", {{"type", "integer"}}}}}}},
              {"required", {"name", "type"}}}}}},
          {"relations",
           {{"type", "array"},
            {"items",
             {{"type", "object"},
              {"properties",
               {{"subject", {{"type", "string"}}},
                {"predicate", {{"type", "string"}}},
                {"object", {{"type", "string"}}},
                {"confidence", {{"type", "number"}}},
                {"valid_from", {{"type", "string"}}},
                {"valid_to", {{"type", "string"}}},
                {"chunk", {{"type", "integer"}}}}},
              {"required", {"subject", "predicate", "object", "chunk"}}}}}}}},
        {"required", {"entities", "relations"}}};
    return s;
}

namespace {

std::string iso_date(const json& j, const char* key) {
    const std::string v = jstr(j, key);
    if (v.size() < 10) return {};
    for (size_t i = 0; i < 10; ++i) {
        const bool digit = v[i] >= '0' && v[i] <= '9';
        if ((i == 4 || i == 7) ? v[i] != '-' : !digit) return {};
    }
    return v.substr(0, 10);
}

std::string predicate_of(std::string_view p) {
    std::string out;
    for (char c : fold_latin(p.substr(0, 128))) {
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) out.push_back(c);
        else if (!out.empty() && out.back() != '_') out.push_back('_');
    }
    while (!out.empty() && out.back() == '_') out.pop_back();
    return out.substr(0, 64);
}

}  // namespace

std::optional<Extraction> parse_extraction(std::string_view text, int chunk_count) {
    std::string_view body = text;
    if (const size_t fence = body.find("```"); fence != std::string_view::npos) {
        body.remove_prefix(fence + 3);
        if (const size_t nl = body.find('\n'); nl != std::string_view::npos && nl < 16) body.remove_prefix(nl + 1);
        if (const size_t close = body.find("```"); close != std::string_view::npos) body = body.substr(0, close);
    }
    const size_t open = body.find('{'), close = body.rfind('}');
    if (open == std::string_view::npos || close == std::string_view::npos || close < open) return std::nullopt;
    auto j = parse_json(body.substr(open, close - open + 1));
    if (!j || !j->is_object()) return std::nullopt;
    Extraction out;
    std::map<std::string, size_t> by_name;  // folded name -> index
    if (const json* es = jobj(*j, "entities"); es && es->is_array()) {
        for (const auto& e : *es) {
            if (out.entities.size() >= 100) break;
            const std::string name = bounded(decode_text(jstr(e, "name")), 200);
            const std::string type = jstr(e, "type");
            if (name.empty() || std::find(entity_types().begin(), entity_types().end(), type) == entity_types().end()) continue;
            ExtractedEntity x{name, type, to_lower(bounded(decode_text(jstr(e, "email")), 200)), {}};
            if (x.email.find('@') == std::string::npos || x.email.find(' ') != std::string::npos) x.email.clear();
            if (const json* cs = jobj(e, "chunks"); cs && cs->is_array())
                for (const auto& c : *cs)
                    if (c.is_number_integer() && c.get<long long>() >= 0 && c.get<long long>() < chunk_count &&
                        x.chunks.size() < 64)
                        x.chunks.push_back(static_cast<int>(c.get<long long>()));
            const std::string key = normalize_name(name);
            if (key.empty()) continue;
            if (const auto it = by_name.find(key); it != by_name.end()) {
                auto& prev = out.entities[it->second];
                for (int c : x.chunks)
                    if (std::find(prev.chunks.begin(), prev.chunks.end(), c) == prev.chunks.end()) prev.chunks.push_back(c);
                if (prev.email.empty()) prev.email = x.email;
                continue;
            }
            by_name[key] = out.entities.size();
            out.entities.push_back(std::move(x));
        }
    }
    if (const json* rs = jobj(*j, "relations"); rs && rs->is_array()) {
        for (const auto& r : *rs) {
            if (out.relations.size() >= 200) break;
            ExtractedRelation x;
            x.subject = bounded(decode_text(jstr(r, "subject")), 200);
            x.object = bounded(decode_text(jstr(r, "object")), 200);
            x.predicate = predicate_of(decode_text(jstr(r, "predicate")));
            const auto s = by_name.find(normalize_name(x.subject)), o = by_name.find(normalize_name(x.object));
            if (x.predicate.empty() || s == by_name.end() || o == by_name.end() || s->second == o->second) continue;
            x.subject = out.entities[s->second].name;
            x.object = out.entities[o->second].name;
            const long long c = jint(r, "chunk", -1);
            if (c < 0 || c >= chunk_count) continue;
            x.chunk = static_cast<int>(c);
            if (const auto it = r.find("confidence"); it != r.end() && it->is_number()) {
                const double v = it->get<double>();
                x.confidence = std::isfinite(v) ? std::clamp(v, 0.0, 1.0) : 0.5;
            }
            x.valid_from = iso_date(r, "valid_from");
            x.valid_to = iso_date(r, "valid_to");
            out.relations.push_back(std::move(x));
        }
    }
    return out;
}

}  // namespace wb::knowledge
