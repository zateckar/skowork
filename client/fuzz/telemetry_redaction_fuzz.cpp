// Fuzz target for telemetry redaction (add-telemetry-and-admin task 3.2; client/telemetry Secret Redaction): the text
// of a record and the policy's redaction patterns are both untrusted input to the record builder. The first line is a
// policy-supplied pattern, the rest is the text. Any input must build a record without a crash or hang; a secret
// planted around the text never survives, neither one the policy pattern names nor a built-in token shape; strings stay
// within their limits and are not cut inside a UTF-8 sequence; the domain reduction yields a host name or nothing.
// Differential: on a short text, the matcher (design D8) replaces exactly what std::regex replaces, for every pattern both
// accept that cannot match the empty string.
#include "fuzz_main.h"
#include "base/redact.h"
#include "telemetry/record.h"

#include <cstdio>
#include <regex>

using namespace wb;
using namespace wb::telemetry;

namespace {

constexpr std::string_view kPlanted = "WBFUZZ-4711";
constexpr std::string_view kBearer = "Bearer abcdefghijklmnopqrstuvwxyz0123456789ABCDEF";

bool cut_cleanly(const std::string& s) {
    // A string built from valid UTF-8 input must not end in the middle of a sequence.
    if (s.empty()) return true;
    size_t i = s.size();
    size_t back = 0;
    while (i > 0 && back < 4 && (static_cast<unsigned char>(s[i - 1]) & 0xC0) == 0x80) {
        --i;
        ++back;
    }
    if (i == 0) return true;
    const auto lead = static_cast<unsigned char>(s[i - 1]);
    const size_t need = lead >= 0xF0 ? 3 : lead >= 0xE0 ? 2 : lead >= 0xC0 ? 1 : 0;
    return need == 0 || back >= need;
}

// A quantified group, such as (a|a*)*: std::regex backtracks exponentially on it even over a short text, so the
// differential check skips it (the matcher itself is still run on the input above, within its budget).
bool quantified_group(std::string_view p) {
    for (size_t i = 0; i + 1 < p.size(); ++i)
        if (p[i] == ')' && std::string_view("*+?{").find(p[i + 1]) != std::string_view::npos) return true;  // escaped or not
    return false;
}

bool valid_utf8(std::string_view s) {
    for (size_t i = 0; i < s.size();) {
        const auto c = static_cast<unsigned char>(s[i]);
        const size_t n = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 0;
        if (n == 0 || i + n > s.size()) return false;
        for (size_t k = 1; k < n; ++k)
            if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) return false;
        i += n;
    }
    return true;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::string_view in(reinterpret_cast<const char*>(data), size);
    const size_t nl = in.find('\n');
    const std::string pattern(nl == std::string_view::npos ? std::string_view{} : in.substr(0, nl));
    const std::string text(nl == std::string_view::npos ? in : in.substr(nl + 1));

    // Policy patterns: the fuzzed one (may not compile) and one naming the planted secret.
    const Redactor redactor({pattern, "WBFUZZ-[0-9]+"});
    if (redactor.invalid().size() > 2) __builtin_trap();

    const std::string metadata_text = std::string(kPlanted) + " " + text + " " + std::string(kBearer);
    const std::string content_text = text + " " + std::string(kPlanted) + " " + std::string(kBearer);
    const Field fields[] = {{Attr::error_code, metadata_text},
                            {Attr::error_component, text},
                            {Attr::error_message, content_text}};
    const auto rec = make_record(Event::client_error, 1, fields, Level::content, redactor);
    if (!rec) __builtin_trap();
    for (const Field& f : rec->attrs) {
        const auto* s = std::get_if<std::string>(&f.value);
        if (!s) __builtin_trap();
        const size_t max = def(f.attr).level == Level::content ? kMaxContentString : kMaxMetadataString;
        if (s->size() > max) __builtin_trap();
        if (valid_utf8(text) && !cut_cleanly(*s)) __builtin_trap();
        // The planted secrets are gone, whatever surrounds them (a cut may also have removed them).
        if (f.attr == Attr::error_code || f.attr == Attr::error_message) {
            if (s->find(kPlanted) != std::string::npos) __builtin_trap();
            // Unless the text itself carries the same characters (the dictionary offers them), which are not the token.
            constexpr std::string_view token = "abcdefghijklmnopqrstuvwxyz0123456789";
            if (text.find(token) == std::string::npos && s->find(token) != std::string::npos) __builtin_trap();
        }
    }

    // Domain reduction: a host name (letters, digits, dots, hyphens, IPv6 colons) or nothing; never a path or query.
    const std::string host = domain_of(text);
    if (host.size() > 253) __builtin_trap();
    for (const char c : host)
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-' || c == ':')) __builtin_trap();
    const Field url{Attr::url_domain, text};
    const auto tool = make_record(Event::tool_call, 1, std::span<const Field>(&url, 1), Level::metadata, redactor);
    if (!tool) __builtin_trap();
    for (const Field& f : tool->attrs)
        if (std::get<std::string>(f.value) != host) __builtin_trap();
    // Differential check against std::regex (backtracking, so only on short texts and patterns).
    if (pattern.size() <= 24 && !quantified_group(pattern)) {
        const std::string shorttext = text.substr(0, 12);
        if (auto mine = Pattern::compile(pattern); mine && !mine->nullable()) {
            std::optional<std::string> theirs;
            try {
                const std::regex re(pattern, std::regex::ECMAScript);
                theirs = std::regex_replace(shorttext, re, "#");
            } catch (const std::regex_error&) {
            }
            long long budget = kRedactionBudget;
            const auto got = mine->replace(shorttext, "#", budget);
            if (!got) __builtin_trap();
            if (theirs && *theirs != *got) {
                std::fprintf(stderr, "pattern [%s] text [%s]: std::regex [%s], Pattern [%s]\n", pattern.c_str(), shorttext.c_str(),
                             theirs->c_str(), got->c_str());
                __builtin_trap();
            }
        }
    }
    return 0;
}