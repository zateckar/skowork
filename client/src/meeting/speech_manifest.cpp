#include "meeting/speech_manifest.h"

#include "base/strings.h"

#include <algorithm>
#include <cctype>
#include <set>

namespace wb::meeting {
namespace {

bool segment_ok(std::string_view s) {
    if (s.empty() || s == "." || s == ".." || s.size() > 96) return false;
    return std::all_of(s.begin(), s.end(), [](unsigned char c) {
        return std::isalnum(c) || c == '_' || c == '.' || c == '-';
    });
}

bool hex64(std::string_view s) {
    return s.size() == 64 && std::all_of(s.begin(), s.end(), [](unsigned char c) {
               return std::isdigit(c) || (c >= 'a' && c <= 'f');
           });
}

}  // namespace

bool speech_version_ok(std::string_view v) {
    if (v.empty() || v.size() > 32 || !std::isalnum(static_cast<unsigned char>(v.front()))) return false;
    return std::all_of(v.begin(), v.end(), [](unsigned char c) {
        return std::isalnum(c) || c == '_' || c == '.' || c == '-';
    });
}

namespace {

// Splits on '/' and keeps empty segments so "a//b" is refused.
std::vector<std::string_view> split(std::string_view s, char sep) {
    std::vector<std::string_view> out;
    size_t start = 0;
    for (size_t i = 0; i <= s.size(); ++i)
        if (i == s.size() || s[i] == sep) out.push_back(s.substr(start, i - start)), start = i + 1;
    return out;
}

}  // namespace

Status check_speech_entry_name(std::string_view name) {
    const auto bad = [&](const char* why) { return Status(make_error("speech_package", std::string("invalid archive entry: ") + why)); };
    if (name.empty() || name.size() > 200) return bad("name length");
    for (unsigned char c : name)
        if (c < 0x20 || c == 0x7f || c == '\\' || c == ':') return bad("character");
    if (name.front() == '/') return bad("absolute path");
    if (name == "manifest.json") return {};
    const auto parts = split(name, '/');
    for (const auto& p : parts)
        if (!segment_ok(p)) return bad("path segment");
    if (parts.size() == 2 && parts[0] == "licenses") return {};
    if (parts.size() >= 3 && parts[0] == "models" && parts[1] == "stt") return {};
    return bad("location");
}

Result<SpeechManifest> parse_speech_manifest(std::string_view text) {
    const auto parsed = parse_json(text);
    if (!parsed || !parsed->is_object()) return make_error("speech_package", "the manifest is not a JSON object");
    const json& j = *parsed;
    if (!j.contains("schema") || !j["schema"].is_number_integer() || j["schema"].get<long long>() != kSpeechSchema)
        return make_error("speech_package", "the package schema is not supported by this client");
    if (jstr(j, "package") != kSpeechPackageName) return make_error("speech_package", "the archive is not a speech package");
    if (jstr(j, "architecture") != kSpeechArchitecture) return make_error("speech_package", "the package is for another architecture");
    SpeechManifest m;
    m.version = jstr(j, "version");
    if (!speech_version_ok(m.version)) return make_error("speech_package", "the package version is invalid");
    const json* files = jobj(j, "files");
    if (!files || !files->is_array() || files->empty() || files->size() > 64)
        return make_error("speech_package", "the manifest lists no usable files");
    std::set<std::string> seen;
    for (const auto& f : *files) {
        if (!f.is_object()) return make_error("speech_package", "a manifest entry is not an object");
        SpeechManifestFile e;
        e.path = jstr(f, "path");
        e.sha256 = jstr(f, "sha256");
        e.source = jstr(f, "source");
        if (!f.contains("bytes") || !f["bytes"].is_number_integer()) return make_error("speech_package", "a manifest entry has no size: " + e.path);
        const long long bytes = f["bytes"].get<long long>();
        if (bytes <= 0 || bytes > (1ll << 40)) return make_error("speech_package", "a manifest entry has an invalid size: " + e.path);
        e.bytes = static_cast<std::uint64_t>(bytes);
        if (!hex64(e.sha256)) return make_error("speech_package", "a manifest entry has an invalid digest: " + e.path);
        if (e.source != "models" && e.source != "runtime") return make_error("speech_package", "a manifest entry has an invalid source");
        const std::string prefix = "models/stt/";
        if (e.path.rfind(prefix, 0) != 0 || !check_speech_entry_name(e.path).ok())
            return make_error("speech_package", "invalid manifest path: " + e.path);
        const std::string rel = e.path.substr(prefix.size());
        if (!seen.insert(to_lower(rel)).second) return make_error("speech_package", "duplicate manifest path: " + e.path);
        m.files.push_back(std::move(e));
    }
    for (const char* req : kSpeechRequiredFiles)
        if (!seen.count(to_lower(std::string("") + req)))
            return make_error("speech_package", std::string("the manifest does not list ") + req);
    return m;
}

int compare_speech_versions(std::string_view a, std::string_view b) {
    const auto pa = split(a, '.'), pb = split(b, '.');
    for (size_t i = 0; i < std::max(pa.size(), pb.size()); ++i) {
        const std::string_view x = i < pa.size() ? pa[i] : std::string_view{};
        const std::string_view y = i < pb.size() ? pb[i] : std::string_view{};
        const bool dx = !x.empty() && std::all_of(x.begin(), x.end(), [](unsigned char c) { return std::isdigit(c); });
        const bool dy = !y.empty() && std::all_of(y.begin(), y.end(), [](unsigned char c) { return std::isdigit(c); });
        if (dx && dy) {
            // Compare by length (no leading zeros are needed for folder names), then lexically.
            const std::string_view tx = x.substr(std::min(x.find_first_not_of('0'), x.size()));
            const std::string_view ty = y.substr(std::min(y.find_first_not_of('0'), y.size()));
            if (tx.size() != ty.size()) return tx.size() < ty.size() ? -1 : 1;
            if (int c = tx.compare(ty); c != 0) return c < 0 ? -1 : 1;
        } else if (int c = x.compare(y); c != 0) {
            return c < 0 ? -1 : 1;
        }
    }
    return 0;
}

}  // namespace wb::meeting
