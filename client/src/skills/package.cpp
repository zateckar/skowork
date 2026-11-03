#include "skills/package.h"

#include <miniz.h>

#include <algorithm>
#include <cctype>
#include <set>
#include <vector>

namespace wb::skills {

namespace {

std::string upper(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return out;
}

bool reserved_device(std::string_view segment) {
    const std::string base = upper(segment.substr(0, segment.find('.')));
    static const std::set<std::string> names = {"CON", "PRN", "AUX", "NUL"};
    if (names.count(base)) return true;
    return base.size() == 4 && (base.starts_with("COM") || base.starts_with("LPT")) && base[3] >= '0' && base[3] <= '9';
}

// The top-level entries a package may have (registry Package rule).
bool allowed_top(std::string_view first, bool is_dir) {
    if (is_dir) return first == "scripts" || first == "references" || first == "assets";
    const std::string u = upper(first);
    return first == "SKILL.md" || u.starts_with("README") || u.starts_with("LICENSE") || u.starts_with("LICENCE");
}

}  // namespace

bool safe_relative_path(std::string_view path) {
    if (path.empty() || path.size() > 260 || path.front() == '/') return false;
    size_t start = 0;
    while (start <= path.size()) {
        const size_t end = std::min(path.find('/', start), path.size());
        const std::string_view seg = path.substr(start, end - start);
        if (seg.empty() || seg == "." || seg == "..") return false;
        if (seg.back() == '.' || seg.back() == ' ') return false;  // Windows drops them, so two names could collide
        for (const char ch : seg) {
            const auto c = static_cast<unsigned char>(ch);
            if (c < 0x20 || c == 0x7F || ch == '\\' || ch == ':' || ch == '*' || ch == '?' || ch == '"' || ch == '<' ||
                ch == '>' || ch == '|')
                return false;
        }
        if (reserved_device(seg)) return false;
        if (end == path.size()) break;
        start = end + 1;
    }
    return true;
}

Result<std::map<std::string, std::string>> read_package(std::string_view zip, const PackageLimits& limits) {
    if (zip.size() > limits.max_bytes) return make_error("too_large", "the package is larger than the limit");
    mz_zip_archive ar{};
    if (!mz_zip_reader_init_mem(&ar, zip.data(), zip.size(), 0)) return make_error("malformed", "the package is not a zip file");
    struct Closer {
        mz_zip_archive* a;
        ~Closer() { mz_zip_reader_end(a); }
    } closer{&ar};
    const mz_uint n = mz_zip_reader_get_num_files(&ar);
    if (n > limits.max_entries) return make_error("too_large", "the package has more than " + std::to_string(limits.max_entries) + " entries");

    std::vector<std::pair<std::string, mz_uint>> files;
    std::set<std::string> seen;
    std::size_t total = 0;
    for (mz_uint i = 0; i < n; ++i) {
        mz_zip_archive_file_stat st{};
        if (!mz_zip_reader_file_stat(&ar, i, &st)) return make_error("malformed", "an entry of the package cannot be read");
        if (st.m_is_encrypted || !st.m_is_supported) return make_error("malformed", "the package has an encrypted or unsupported entry");
        std::string name = st.m_filename;
        const bool dir = st.m_is_directory != 0;
        if (dir && !name.empty() && name.back() == '/') name.pop_back();
        if (!safe_relative_path(name)) return make_error("unsafe_path", "the package has an unsafe path: " + name);
        if (!seen.insert(upper(name)).second) return make_error("duplicate", "the package has the entry " + name + " twice");
        if (dir) continue;
        total += static_cast<std::size_t>(st.m_uncomp_size);
        if (st.m_uncomp_size > limits.max_unpacked || total > limits.max_unpacked)
            return make_error("too_large", "the package unpacks to more than the limit");
        files.emplace_back(std::move(name), i);
    }

    // SKILL.md at the root, or every file inside one top-level folder that holds it.
    std::string prefix;
    const bool at_root = std::any_of(files.begin(), files.end(), [](const auto& f) { return f.first == "SKILL.md"; });
    if (!at_root) {
        if (files.empty()) return make_error("no_skill_md", "the package has no SKILL.md");
        const std::string first = files.front().first;
        const size_t slash = first.find('/');
        if (slash == std::string::npos) return make_error("no_skill_md", "the package has no SKILL.md");
        prefix = first.substr(0, slash + 1);
        for (const auto& f : files)
            if (!f.first.starts_with(prefix)) return make_error("layout", "SKILL.md must be at the root or in one top-level folder");
        if (!std::any_of(files.begin(), files.end(), [&](const auto& f) { return f.first == prefix + "SKILL.md"; }))
            return make_error("no_skill_md", "the package has no SKILL.md");
    }

    std::map<std::string, std::string> out;
    for (const auto& [name, index] : files) {
        const std::string rel = name.substr(prefix.size());
        const size_t slash = rel.find('/');
        if (!allowed_top(rel.substr(0, slash), slash != std::string::npos))
            return make_error("layout", "the package may not contain " + rel);
        mz_zip_archive_file_stat st{};
        mz_zip_reader_file_stat(&ar, index, &st);
        std::string data(static_cast<size_t>(st.m_uncomp_size), '\0');
        // The declared size is the buffer: an entry that inflates to more fails instead of growing.
        if (!data.empty() && !mz_zip_reader_extract_to_mem(&ar, index, data.data(), data.size(), 0))
            return make_error("malformed", "the entry " + name + " cannot be decompressed");
        out.emplace(rel, std::move(data));
    }
    return out;
}

}  // namespace wb::skills
