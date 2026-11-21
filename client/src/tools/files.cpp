#include "tools/files.h"

#include "base/encoding.h"
#include "base/strings.h"
#include "platform/crypto.h"
#include "platform/win.h"
#include "policy/paths.h"
#include "tools/text.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <regex>
#include <unordered_map>

namespace wb::tools {

using runtime::Provenance;
using runtime::Risk;
using runtime::ToolContext;
using runtime::ToolDef;
using runtime::ToolOutput;
using runtime::ToolResource;

namespace {

constexpr size_t kMaxWalkEntries = 50000;
constexpr long long kMaxGrepFileBytes = 8ll << 20;
constexpr size_t kMaxGrepLine = 4096;

bool is_absolute(std::string_view p) {
    return (p.size() >= 3 && std::isalpha(static_cast<unsigned char>(p[0])) && p[1] == ':' && (p[2] == '\\' || p[2] == '/')) ||
           (p.size() >= 2 && (p[0] == '\\' || p[0] == '/') && (p[1] == '\\' || p[1] == '/'));
}

std::string arg_path(const json& args, const char* key, const std::string& workspace) {
    const std::string p = jstr(args, key);
    return absolute_path(p.empty() ? workspace : p, workspace);
}

// Calls of files.read, files.write and files.edit on one file run one at a time, so parallel edits of a turn each apply to
// the content the previous one left (fix-stream-gaps-and-parallel-file-edits D3). One mutex per path, process-wide.
struct PathLock {
    std::shared_ptr<std::mutex> mutex;
    std::unique_lock<std::mutex> lock;
};

PathLock lock_path(const std::string& path) {
    static std::mutex registry_mx;
    static std::unordered_map<std::string, std::weak_ptr<std::mutex>> registry;
    std::string key = to_lower(path);
    std::replace(key.begin(), key.end(), '/', '\\');
    std::shared_ptr<std::mutex> m;
    {
        const std::lock_guard g(registry_mx);
        if (registry.size() > 1024) std::erase_if(registry, [](const auto& e) { return e.second.expired(); });
        auto& slot = registry[key];
        m = slot.lock();
        if (!m) {
            m = std::make_shared<std::mutex>();
            slot = m;
        }
    }
    std::unique_lock<std::mutex> l(*m);
    return {std::move(m), std::move(l)};
}

Provenance files_provenance(const std::string& id) { return {"files", id, "", false}; }

ToolOutput with_provenance(ToolOutput out, const std::string& id) {
    out.provenance = files_provenance(id);
    return out;
}

// A failure from the path API, phrased for the model.
ToolOutput path_failure(const Error& e, const std::string& path) {
    if (e.code == "policy_denied") return ToolOutput::failure("Access to " + path + " is denied by policy (" + e.message + ").");
    return ToolOutput::failure("Cannot access " + path + ": " + e.message);
}

long long policy_int(const FileToolsEnv& env, const char* key, long long fallback) {
    const auto p = env.policy ? env.policy() : nullptr;
    return p ? p->integer(key, fallback) : fallback;
}

Result<std::string> read_all(HANDLE h, long long max_bytes, long long* total = nullptr) {
    LARGE_INTEGER size{};
    if (!::GetFileSizeEx(h, &size)) return make_error("io", "cannot read the file size: " + error_text(::GetLastError()));
    if (total) *total = size.QuadPart;
    const long long want = std::min<long long>(size.QuadPart, max_bytes);
    std::string data(static_cast<size_t>(want), '\0');
    size_t got = 0;
    while (got < data.size()) {
        DWORD n = 0;
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(data.size() - got, 1u << 20));
        if (!::ReadFile(h, data.data() + got, chunk, &n, nullptr)) return make_error("io", "cannot read the file: " + error_text(::GetLastError()));
        if (n == 0) break;
        got += n;
    }
    data.resize(got);
    return data;
}

bool is_directory(HANDLE h) {
    BY_HANDLE_FILE_INFORMATION info{};
    return ::GetFileInformationByHandle(h, &info) && (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
}

// ---- Seen File Content (protect-unread-overwrites) ------------------------------------------------------------------

std::string sha256_hex(std::string_view bytes) { return hex_encode(sha256(bytes)); }

long long last_write_of(HANDLE h) {
    BY_HANDLE_FILE_INFORMATION info{};
    if (!::GetFileInformationByHandle(h, &info)) return 0;
    return (static_cast<long long>(info.ftLastWriteTime.dwHighDateTime) << 32) | info.ftLastWriteTime.dwLowDateTime;
}

// Size and last-write time of an existing file, without opening it for reading; nothing when there is no such file.
struct DiskStat {
    long long size = 0, modified = 0;
    bool directory = false;
};
std::optional<DiskStat> stat_path(const std::string& path, bool* missing = nullptr) {
    WIN32_FILE_ATTRIBUTE_DATA d{};
    const std::wstring w = widen(path);
    if (!::GetFileAttributesExW(w.c_str(), GetFileExInfoStandard, &d)) {
        const DWORD err = ::GetLastError();
        if (missing) *missing = err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND;
        return std::nullopt;
    }
    if (missing) *missing = false;
    DiskStat s;
    s.size = (static_cast<long long>(d.nFileSizeHigh) << 32) | d.nFileSizeLow;
    s.modified = (static_cast<long long>(d.ftLastWriteTime.dwHighDateTime) << 32) | d.ftLastWriteTime.dwLowDateTime;
    s.directory = (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    return s;
}

// The state of the file just written: its stat on disk and the hash of the bytes written.
void record_written(SeenFiles& seen, const std::string& thread_id, const std::string& written, std::string_view bytes) {
    const auto st = stat_path(written);
    if (!st) return;
    // The same bytes the overwrite check hashes: at most the first kMaxFileBytes.
    seen.put(thread_id, seen_key(written), FileState{st->size, st->modified, sha256_hex(bytes.substr(0, static_cast<size_t>(kMaxFileBytes)))});
}

class MemorySeenFiles final : public SeenFiles {
public:
    std::optional<FileState> get(const std::string& thread_id, const std::string& path) override {
        const std::lock_guard g(mx_);
        const auto it = states_.find(thread_id + '\n' + path);
        if (it == states_.end()) return std::nullopt;
        return it->second;
    }
    void put(const std::string& thread_id, const std::string& path, const FileState& state) override {
        const std::lock_guard g(mx_);
        states_[thread_id + '\n' + path] = state;
    }

private:
    std::mutex mx_;
    std::unordered_map<std::string, FileState> states_;
};

struct DirEntry {
    std::string name;
    bool dir = false;
    bool link = false;
    long long size = 0;
    long long modified = 0;  // FILETIME as 100 ns ticks
};

Result<std::vector<DirEntry>> read_directory(HANDLE dir) {
    std::vector<DirEntry> out;
    std::vector<unsigned char> buf(64 * 1024);
    bool first = true;
    for (;;) {
        if (!::GetFileInformationByHandleEx(dir, first ? FileIdBothDirectoryRestartInfo : FileIdBothDirectoryInfo, buf.data(),
                                            static_cast<DWORD>(buf.size()))) {
            const DWORD e = ::GetLastError();
            if (e == ERROR_NO_MORE_FILES) break;
            return make_error("io", "cannot list the folder: " + error_text(e));
        }
        first = false;
        for (size_t off = 0;;) {
            const auto* info = reinterpret_cast<const FILE_ID_BOTH_DIR_INFO*>(buf.data() + off);
            const std::wstring_view wname(info->FileName, info->FileNameLength / sizeof(wchar_t));
            if (wname != L"." && wname != L"..") {
                DirEntry d;
                d.name = narrow(wname);
                d.dir = (info->FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                d.link = (info->FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
                d.size = info->EndOfFile.QuadPart;
                d.modified = info->LastWriteTime.QuadPart;
                out.push_back(std::move(d));
            }
            if (info->NextEntryOffset == 0) break;
            off += info->NextEntryOffset;
        }
    }
    std::sort(out.begin(), out.end(), [](const DirEntry& a, const DirEntry& b) {
        if (a.dir != b.dir) return a.dir;
        return to_lower(a.name) < to_lower(b.name);
    });
    return out;
}

std::string format_time(long long ticks) {
    if (ticks <= 0) return {};
    FILETIME ft{static_cast<DWORD>(ticks & 0xFFFFFFFF), static_cast<DWORD>(ticks >> 32)};
    SYSTEMTIME st{};
    if (!::FileTimeToSystemTime(&ft, &st)) return {};
    char b[32];
    std::snprintf(b, sizeof b, "%04u-%02u-%02uT%02u:%02uZ", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
    return b;
}

std::string join_path(const std::string& dir, const std::string& name) {
    return dir.empty() || dir.back() == '\\' ? dir + name : dir + "\\" + name;
}

// Walks a folder tree below `root` (a final path). Entries the rules deny are counted, not visited; links to folders
// are not followed. `visit` gets the final path and the path relative to the root; it returns false to stop.
struct WalkStats {
    size_t omitted = 0, links = 0, entries = 0, unreadable = 0;
    bool stopped = false;
};

void walk(const std::string& dir, const std::string& rel, const policy::PathRules& rules, WalkStats& st, int depth,
          const std::function<bool(const std::string& path, const std::string& rel, const DirEntry& e)>& visit) {
    if (st.stopped || depth > 64) return;
    auto h = policy::open_for_read(dir, rules);
    if (!h) {
        ++st.unreadable;
        return;
    }
    auto entries = read_directory(h->get());
    if (!entries) {
        ++st.unreadable;
        return;
    }
    h->reset();
    for (const auto& e : *entries) {
        if (st.stopped) return;
        if (++st.entries > kMaxWalkEntries) {
            st.stopped = true;
            return;
        }
        const std::string path = join_path(dir, e.name);
        const std::string child_rel = rel.empty() ? e.name : rel + "\\" + e.name;
        if (!policy::check_path(path, policy::Access::read, rules).allowed) {
            ++st.omitted;
            continue;
        }
        if (e.dir && e.link) {
            ++st.links;
            continue;
        }
        if (!visit(path, child_rel, e)) {
            st.stopped = true;
            return;
        }
        if (e.dir) walk(path, child_rel, rules, st, depth + 1, visit);
    }
}

std::string normalise_pattern(std::string p) {
    std::replace(p.begin(), p.end(), '/', '\\');
    while (starts_with(p, ".\\")) p.erase(0, 2);
    return p;
}

std::string walk_footer(const WalkStats& st) {
    std::string s;
    if (st.omitted) s += "\n(" + std::to_string(st.omitted) + " entries omitted because policy denies reading them)";
    if (st.links) s += "\n(" + std::to_string(st.links) + " linked folders not followed)";
    if (st.unreadable) s += "\n(" + std::to_string(st.unreadable) + " folders could not be read)";
    return s;
}

std::vector<ToolResource> read_resource(const json& args, const char* key, const std::string& workspace) {
    return {{ToolResource::Kind::read_path, arg_path(args, key, workspace)}};
}

std::string lf_to_crlf(std::string_view s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\n' && (i == 0 || s[i - 1] != '\r')) out += '\r';
        out += s[i];
    }
    return out;
}

json schema(json properties, std::vector<std::string> required) {
    return {{"type", "object"}, {"properties", std::move(properties)}, {"required", std::move(required)}};
}

// ---- files.read ----------------------------------------------------------------------------------------------------

ToolOutput read_file(const FileToolsEnv& env, ToolContext& ctx, const json& args) {
    const std::string path = arg_path(args, "path", env.workspace);
    if (jstr(args, "path").empty()) return ToolOutput::failure("path is required.");
    const PathLock locked = lock_path(path);
    auto h = policy::open_for_read(path, env.rules());
    if (!h) return path_failure(h.error(), path);
    if (is_directory(h->get())) return ToolOutput::failure(path + " is a folder; use files.list.");
    auto final_path = policy::final_path_of(h->get());
    const std::string shown = final_path ? *final_path : path;
    long long total = 0;
    auto data = read_all(h->get(), kMaxFileBytes, &total);
    if (!data) return ToolOutput::failure(data.error().message);

    std::string text;
    const std::string kind = document_kind(shown);
    if (!kind.empty()) {
        if (!env.extract) return ToolOutput::failure("Text extraction for " + kind + " documents is not available.");
        if (total > kMaxFileBytes) return ToolOutput::failure("The document is larger than " + std::to_string(kMaxFileBytes >> 20) + " MB.");
        auto extracted = env.extract(kind, *data, ctx);
        if (!extracted) return with_provenance(ToolOutput::failure("Text extraction failed: " + extracted.error().message), shown);
        text = std::move(*extracted);
    } else {
        auto decoded = decode_text(*data);
        if (!decoded) return ToolOutput::failure("binary file not supported: " + shown + " is not a text file or a supported document.");
        text = std::move(decoded->text);
    }

    const long long max_bytes = std::max<long long>(1, policy_int(env, "tools.files.max_read_kb", 256)) * 1024;
    const bool ranged = args.contains("offset") || args.contains("limit");
    const size_t first = static_cast<size_t>(std::max<long long>(1, jint(args, "offset", 1)));
    const size_t limit = static_cast<size_t>(std::max<long long>(0, jint(args, "limit", 0)));
    const size_t lines = count_lines(text);
    // A full read of a text file counts as seen (Seen File Content); ranged, truncated and extracted reads do not.
    auto mark_seen = [&] {
        if (kind.empty() && !ranged && total <= kMaxFileBytes && env.seen)
            env.seen->put(ctx.thread_id, seen_key(shown), FileState{total, last_write_of(h->get()), sha256_hex(*data)});
    };
    if (text.empty()) {
        mark_seen();
        return with_provenance(ToolOutput::success("(empty file)"), shown);
    }
    if (first > lines) {
        return with_provenance(ToolOutput::failure("offset " + std::to_string(first) + " is beyond the end of " + shown + ", which has " +
                                                   std::to_string(lines) + " lines."),
                               shown);
    }
    LineSlice s = slice_lines(text, first, limit, static_cast<size_t>(max_bytes), ranged);
    std::string out = std::move(s.text);
    const bool partial_file = total > kMaxFileBytes;
    if (s.truncated || (!ranged && s.last < lines)) {
        if (!out.empty() && out.back() != '\n') out += '\n';
        out += "[Truncated: showing lines " + std::to_string(s.first) + "-" + std::to_string(s.last) + " of " + std::to_string(lines) +
               (partial_file ? "+" : "") + " (the file has " + std::to_string(total) + " bytes; the read limit is " +
               std::to_string(max_bytes / 1024) + " KB). Call files.read with offset=" + std::to_string(s.last + 1) +
               " and a limit to read further ranges.]";
    } else {
        mark_seen();
    }
    return with_provenance(ToolOutput::success(std::move(out)), shown);
}

// ---- files.list ----------------------------------------------------------------------------------------------------

ToolOutput list_folder(const FileToolsEnv& env, const json& args) {
    const std::string path = arg_path(args, "path", env.workspace);
    const auto rules = env.rules();
    auto h = policy::open_for_read(path, rules);
    if (!h) return path_failure(h.error(), path);
    if (!is_directory(h->get())) return ToolOutput::failure(path + " is a file; use files.read.");
    auto dir = policy::final_path_of(h->get());
    if (!dir) return ToolOutput::failure(dir.error().message);
    auto entries = read_directory(h->get());
    if (!entries) return ToolOutput::failure(entries.error().message);
    std::string out = *dir + "\n";
    size_t omitted = 0, shown = 0;
    for (const auto& e : *entries) {
        if (!policy::check_path(join_path(*dir, e.name), policy::Access::read, rules).allowed) {
            ++omitted;
            continue;
        }
        if (++shown > 2000) continue;
        out += e.dir ? "[dir]  " : (e.link ? "[link] " : "[file] ");
        out += e.name;
        if (e.dir) out += "\\";
        else out += "  " + std::to_string(e.size) + " bytes";
        if (const std::string t = format_time(e.modified); !t.empty()) out += "  " + t;
        out += "\n";
    }
    if (shown == 0) out += "(empty)\n";
    if (shown > 2000) out += "(" + std::to_string(shown - 2000) + " more entries not shown)\n";
    if (omitted) out += "(" + std::to_string(omitted) + " entries omitted because policy denies reading them)\n";
    return with_provenance(ToolOutput::success(std::move(out)), *dir);
}

// ---- files.glob ----------------------------------------------------------------------------------------------------

ToolOutput glob_files(const FileToolsEnv& env, const json& args) {
    const std::string pattern = normalise_pattern(jstr(args, "pattern"));
    if (pattern.empty()) return ToolOutput::failure("pattern is required.");
    const std::string path = arg_path(args, "path", env.workspace);
    const auto rules = env.rules();
    auto h = policy::open_for_read(path, rules);
    if (!h) return path_failure(h.error(), path);
    auto root = policy::final_path_of(h->get());
    if (!root) return ToolOutput::failure(root.error().message);
    h->reset();
    const size_t max = static_cast<size_t>(std::clamp<long long>(jint(args, "max_results", 500), 1, 5000));
    // A pattern without a separator matches file names at any depth, like "**\pattern".
    const bool by_name = pattern.find('\\') == std::string::npos;
    std::vector<std::string> hits;
    bool more = false;
    WalkStats st;
    walk(*root, "", rules, st, 0, [&](const std::string& p, const std::string& rel, const DirEntry& e) {
        const bool match = by_name ? policy::glob_match(pattern, e.name) : policy::glob_match(pattern, rel);
        if (match) {
            if (hits.size() >= max) {
                more = true;
                return false;
            }
            hits.push_back(e.dir ? p + "\\" : p);
        }
        return true;
    });
    std::string out = std::to_string(hits.size()) + (more ? "+" : "") + " matches for " + pattern + " under " + *root + "\n";
    for (const auto& p : hits) out += p + "\n";
    if (more) out += "(more matches not shown; narrow the pattern or raise max_results)\n";
    if (st.stopped && !more) out += "(the search stopped after " + std::to_string(kMaxWalkEntries) + " entries)\n";
    out += walk_footer(st);
    return with_provenance(ToolOutput::success(std::move(out)), *root);
}

// ---- files.grep ----------------------------------------------------------------------------------------------------

ToolOutput grep_files(const FileToolsEnv& env, const json& args) {
    const std::string expr = jstr(args, "pattern");
    if (expr.empty()) return ToolOutput::failure("pattern is required.");
    std::regex re;
    try {
        auto flags = std::regex::ECMAScript | std::regex::optimize;
        if (jbool(args, "case_insensitive")) flags |= std::regex::icase;
        re = std::regex(expr, flags);
    } catch (const std::regex_error& e) {
        return ToolOutput::failure(std::string("The regular expression is not valid: ") + e.what());
    }
    const std::string filter = normalise_pattern(jstr(args, "glob"));
    const bool filter_by_name = filter.find('\\') == std::string::npos;
    const std::string path = arg_path(args, "path", env.workspace);
    const auto rules = env.rules();
    auto h = policy::open_for_read(path, rules);
    if (!h) return path_failure(h.error(), path);
    auto root = policy::final_path_of(h->get());
    if (!root) return ToolOutput::failure(root.error().message);
    const bool single = !is_directory(h->get());
    h->reset();
    const size_t max = static_cast<size_t>(std::clamp<long long>(jint(args, "max_results", 200), 1, 2000));
    std::string out;
    size_t matches = 0, files = 0, skipped = 0;
    bool more = false;

    auto search = [&](const std::string& file) -> bool {
        auto fh = policy::open_for_read(file, rules);
        if (!fh) return true;
        LARGE_INTEGER size{};
        if (!::GetFileSizeEx(fh->get(), &size) || size.QuadPart > kMaxGrepFileBytes) {
            ++skipped;
            return true;
        }
        auto data = read_all(fh->get(), kMaxGrepFileBytes);
        if (!data) return true;
        auto text = decode_text(*data);
        if (!text) return true;  // binary
        ++files;
        size_t line = 0, pos = 0;
        const std::string& t = text->text;
        while (pos < t.size()) {
            ++line;
            size_t nl = t.find('\n', pos);
            if (nl == std::string::npos) nl = t.size();
            std::string_view l(t.data() + pos, nl - pos);
            if (!l.empty() && l.back() == '\r') l.remove_suffix(1);
            pos = nl + 1;
            const std::string_view probe = l.substr(0, kMaxGrepLine);
            if (std::regex_search(probe.begin(), probe.end(), re)) {
                if (matches >= max) {
                    more = true;
                    return false;
                }
                ++matches;
                out += file + ":" + std::to_string(line) + ": " + utf8_truncate(l, 500) + "\n";
            }
        }
        return true;
    };

    WalkStats st;
    if (single) {
        search(*root);
    } else {
        walk(*root, "", rules, st, 0, [&](const std::string& p, const std::string& rel, const DirEntry& e) {
            if (e.dir) return true;
            if (!filter.empty() && !(filter_by_name ? policy::glob_match(filter, e.name) : policy::glob_match(filter, rel))) return true;
            return search(p);
        });
    }
    std::string head = std::to_string(matches) + (more ? "+" : "") + " matches in " + std::to_string(files) + " files under " + *root + "\n";
    if (matches == 0) head += "(no matches)\n";
    out = head + out;
    if (more) out += "(more matches not shown; narrow the pattern or raise max_results)\n";
    if (skipped) out += "(" + std::to_string(skipped) + " files larger than 8 MB not searched)\n";
    out += walk_footer(st);
    return with_provenance(ToolOutput::success(std::move(out)), *root);
}

// ---- files.write and files.edit ------------------------------------------------------------------------------------

// client/core-tools Overwrite Protection (protect-unread-overwrites design D3, D4): an existing file may be replaced only
// when its current content is what the agent last saw of it in this Thread. Nothing when the write may go ahead.
std::optional<std::string> overwrite_refusal(const FileToolsEnv& env, const ToolContext& ctx, const std::string& path,
                                             const policy::PathRules& rules) {
    // A path the policy does not let the agent write is refused by the write itself, without telling whether it exists.
    const auto canonical = policy::canonicalize(path);
    if (!canonical || !policy::check_path(*canonical, policy::Access::write, rules).allowed) return std::nullopt;
    bool missing = false;
    const auto st = stat_path(*canonical, &missing);
    if (!st && missing) return std::nullopt;  // a new file
    // Any other failure (access denied, a sharing error) fails closed: the file may exist.
    if (!st) return path + " could not be checked before replacing it. The file is unchanged.";
    if (st->directory) return path + " is a folder.";
    const long long max_read = std::max<long long>(1, policy_int(env, "tools.files.max_read_kb", 256)) * 1024;
    const std::string cannot = path + " is too large or not a text file, so it cannot be replaced with files.write. Use files.edit "
                                      "or write to a new file. The file is unchanged.";
    const auto seen = env.seen ? env.seen->get(ctx.thread_id, seen_key(path)) : std::nullopt;
    if (!seen) {
        if (!document_kind(path).empty() || st->size > max_read) return cannot;
        return path + " exists and has not been read in this Thread. Read it in full with files.read before replacing it, or "
                      "write to a new file. The file is unchanged.";
    }
    const std::string changed = path + " changed after it was last read in this Thread. Read it again before replacing it. "
                                       "The file is unchanged.";
    if (st->size != seen->size || st->modified != seen->modified) return changed;
    auto h = policy::open_for_read(path, rules);
    if (!h) return path + " cannot be read to check it before replacing it, so it is not replaced. The file is unchanged.";
    auto data = read_all(h->get(), kMaxFileBytes);
    if (!data || sha256_hex(*data) != seen->sha256) return changed;
    return std::nullopt;
}

ToolOutput write_file(const FileToolsEnv& env, ToolContext& ctx, const json& args) {
    if (jstr(args, "path").empty()) return ToolOutput::failure("path is required.");
    if (!args.contains("content") || !args["content"].is_string()) return ToolOutput::failure("content is required.");
    const std::string path = arg_path(args, "path", env.workspace);
    const std::string& content = args["content"].get_ref<const std::string&>();
    const auto rules = env.rules();
    const PathLock locked = lock_path(path);
    if (auto refused = overwrite_refusal(env, ctx, path, rules)) return ToolOutput::failure(*refused);
    auto written = policy::write_file_atomic(path, content, rules);
    if (!written) return path_failure(written.error(), path);
    if (env.seen) record_written(*env.seen, ctx.thread_id, *written, content);
    return ToolOutput::success("Wrote " + std::to_string(content.size()) + " bytes to " + *written + ".");
}

ToolOutput edit_file(const FileToolsEnv& env, ToolContext& ctx, const json& args) {
    if (jstr(args, "path").empty()) return ToolOutput::failure("path is required.");
    const std::string old_text = jstr(args, "old_text");
    if (old_text.empty()) return ToolOutput::failure("old_text is required and must not be empty.");
    if (!args.contains("new_text") || !args["new_text"].is_string()) return ToolOutput::failure("new_text is required.");
    std::string new_text = jstr(args, "new_text");
    const std::string path = arg_path(args, "path", env.workspace);
    const auto rules = env.rules();
    const PathLock locked = lock_path(path);  // read, change and write as one step
    std::string data;
    {
        auto h = policy::open_for_read(path, rules);
        if (!h) return path_failure(h.error(), path);
        if (is_directory(h->get())) return ToolOutput::failure(path + " is a folder.");
        long long total = 0;
        auto d = read_all(h->get(), kMaxFileBytes, &total);
        if (!d) return ToolOutput::failure(d.error().message);
        if (total > kMaxFileBytes) return ToolOutput::failure("The file is too large to edit.");
        data = std::move(*d);
    }
    auto decoded = decode_text(data);
    if (!decoded) return ToolOutput::failure("binary file not supported: " + path + " is not a text file.");
    std::string& text = decoded->text;
    std::string needle = old_text;
    size_t n = count_occurrences(text, needle);
    if (n == 0 && text.find("\r\n") != std::string::npos && needle.find('\n') != std::string::npos) {
        // The model usually writes \n; the file uses \r\n.
        needle = lf_to_crlf(old_text);
        new_text = lf_to_crlf(new_text);
        n = count_occurrences(text, needle);
    }
    const bool all = jbool(args, "replace_all");
    if (n == 0) return ToolOutput::failure("not found: old_text was not found in " + path + "; the file is unchanged.");
    if (n > 1 && !all) {
        return ToolOutput::failure("ambiguous match: old_text occurs " + std::to_string(n) + " times in " + path +
                                   "; include more surrounding text so that it occurs exactly once, or set replace_all to "
                                   "replace every occurrence. The file is unchanged.");
    }
    // Whether the agent had seen the whole file before this edit: only then is the edited file seen as a whole.
    std::optional<FileState> before;
    if (env.seen) before = env.seen->get(ctx.thread_id, seen_key(path));
    const bool was_seen = before && before->size == static_cast<long long>(data.size()) && before->sha256 == sha256_hex(data);
    const size_t at = text.find(needle);
    if (all) {
        // Edit All Occurrences (add-edit-replace-all design D2): non-overlapping, left to right, one write.
        std::string out;
        out.reserve(text.size() + n * new_text.size());
        size_t from = 0;
        for (size_t pos = text.find(needle); pos != std::string::npos; pos = text.find(needle, from)) {
            out.append(text, from, pos - from);
            out += new_text;
            from = pos + needle.size();
        }
        out.append(text, from, std::string::npos);
        text = std::move(out);
    } else {
        text.replace(at, needle.size(), new_text);
    }
    const std::string bytes = encode_text(text, decoded->encoding);
    auto written = policy::write_file_atomic(path, bytes, rules);
    if (!written) return path_failure(written.error(), path);
    if (was_seen) record_written(*env.seen, ctx.thread_id, *written, bytes);
    if (all) return ToolOutput::success("Replaced " + std::to_string(n) + (n == 1 ? " occurrence" : " occurrences") + " in " + *written + ".");
    const size_t line = 1 + static_cast<size_t>(std::count(text.begin(), text.begin() + static_cast<long>(at), '\n'));
    return ToolOutput::success("Edited " + *written + " at line " + std::to_string(line) + ".");
}

}  // namespace

std::string absolute_path(std::string_view path, const std::string& workspace) {
    std::string p = policy::expand_environment(trim(path));
    if (p.empty()) return workspace;
    if (is_absolute(p)) return p;
    return join_path(workspace, p);
}

std::shared_ptr<SeenFiles> memory_seen_files() { return std::make_shared<MemorySeenFiles>(); }

std::string seen_key(std::string_view path) {
    auto c = policy::canonicalize(path);
    std::string key = to_lower(c ? *c : std::string(path));
    std::replace(key.begin(), key.end(), '/', '\\');
    return key;
}

void record_seen_file(SeenFiles& seen, const std::string& thread_id, const std::string& path) {
    const unique_file h(::CreateFileW(widen(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                      nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!h) return;
    long long total = 0;
    auto data = read_all(h.get(), kMaxFileBytes, &total);
    if (!data || total > kMaxFileBytes) return;
    seen.put(thread_id, seen_key(path), FileState{total, last_write_of(h.get()), sha256_hex(*data)});
}

std::vector<ToolDef> file_tools(FileToolsEnv env) {
    if (!env.seen) env.seen = memory_seen_files();
    auto e = std::make_shared<FileToolsEnv>(std::move(env));
    const std::string ws = e->workspace;
    std::vector<ToolDef> tools;
    const json path_prop = {{"type", "string"}, {"description", "Absolute path, or a path relative to the agent workspace."}};

    ToolDef read;
    read.id = "files.read";
    read.description = "Read a text file or the text of a document (docx, xlsx, pptx, pdf). Optionally a line range: offset is the "
                       "first line (1-based), limit the number of lines; a range returns numbered lines. Long files are truncated "
                       "at the policy read limit with a notice that says how to read further.";
    read.parameters = schema({{"path", path_prop},
                              {"offset", {{"type", "integer"}, {"minimum", 1}, {"description", "First line to read (1-based)."}}},
                              {"limit", {{"type", "integer"}, {"minimum", 1}, {"description", "Number of lines to read."}}}},
                             {"path"});
    read.risk = Risk::read;
    read.core = true;
    read.provenance = files_provenance("");
    read.resources_of = [ws](const json& a) { return read_resource(a, "path", ws); };
    read.run = [e](ToolContext& ctx, const json& a) { return read_file(*e, ctx, a); };
    tools.push_back(std::move(read));

    ToolDef list;
    list.id = "files.list";
    list.description = "List the files and folders in a folder (default: the agent workspace), with sizes and modification times.";
    list.parameters = schema({{"path", path_prop}}, {});
    list.risk = Risk::read;
    list.provenance = files_provenance("");
    list.resources_of = [ws](const json& a) { return read_resource(a, "path", ws); };
    list.run = [e](ToolContext&, const json& a) { return list_folder(*e, a); };
    tools.push_back(std::move(list));

    ToolDef glob;
    glob.id = "files.glob";
    glob.description = "Find files and folders by glob pattern under a folder (default: the agent workspace). `*` matches within a "
                       "name, `**` across folders, `?` one character; a pattern without a separator matches names at any depth.";
    glob.parameters = schema({{"pattern", {{"type", "string"}, {"description", "For example **\\*.cpp or src\\*.json or *.log."}}},
                              {"path", path_prop},
                              {"max_results", {{"type", "integer"}, {"minimum", 1}, {"maximum", 5000}}}},
                             {"pattern"});
    glob.risk = Risk::read;
    glob.provenance = files_provenance("");
    glob.resources_of = [ws](const json& a) { return read_resource(a, "path", ws); };
    glob.run = [e](ToolContext&, const json& a) { return glob_files(*e, a); };
    tools.push_back(std::move(glob));

    ToolDef grep;
    grep.id = "files.grep";
    grep.description = "Search the contents of text files by regular expression (ECMAScript syntax) under a folder or in one file "
                       "(default: the agent workspace). Returns path:line: text for each matching line. `glob` limits the files.";
    grep.parameters = schema({{"pattern", {{"type", "string"}, {"description", "Regular expression."}}},
                              {"path", path_prop},
                              {"glob", {{"type", "string"}, {"description", "Only files matching this glob, for example *.cpp."}}},
                              {"case_insensitive", {{"type", "boolean"}}},
                              {"max_results", {{"type", "integer"}, {"minimum", 1}, {"maximum", 2000}}}},
                             {"pattern"});
    grep.risk = Risk::read;
    grep.provenance = files_provenance("");
    grep.resources_of = [ws](const json& a) { return read_resource(a, "path", ws); };
    grep.run = [e](ToolContext&, const json& a) { return grep_files(*e, a); };
    tools.push_back(std::move(grep));

    ToolDef write;
    write.id = "files.write";
    write.description = "Create a file or replace its whole content with `content` (UTF-8). The folder must exist. The write is atomic. "
                        "An existing file can be replaced only after you read it in full with files.read in this Thread, or wrote "
                        "it yourself, and only if it has not changed since.";
    write.parameters = schema({{"path", path_prop}, {"content", {{"type", "string"}}}}, {"path", "content"});
    write.risk = Risk::write_local;
    write.core = true;
    write.resources_of = [ws](const json& a) { return std::vector<ToolResource>{{ToolResource::Kind::write_path, arg_path(a, "path", ws)}}; };
    write.run = [e](ToolContext& ctx, const json& a) { return write_file(*e, ctx, a); };
    tools.push_back(std::move(write));

    ToolDef edit;
    edit.id = "files.edit";
    edit.description = "Replace one exact text span in a text file. old_text must occur exactly once; otherwise the call fails and "
                       "the file is unchanged. Include enough surrounding text to make it unique. Set replace_all to replace every "
                       "occurrence instead, for example to rename something; the result states how many were replaced.";
    edit.parameters = schema({{"path", path_prop},
                              {"old_text", {{"type", "string"}}},
                              {"new_text", {{"type", "string"}}},
                              {"replace_all", {{"type", "boolean"}, {"description", "Replace every occurrence (default false)."}}}},
                             {"path", "old_text", "new_text"});
    edit.risk = Risk::write_local;
    edit.core = true;
    edit.resources_of = [ws](const json& a) {
        const std::string p = arg_path(a, "path", ws);
        return std::vector<ToolResource>{{ToolResource::Kind::read_path, p}, {ToolResource::Kind::write_path, p}};
    };
    edit.run = [e](ToolContext& ctx, const json& a) { return edit_file(*e, ctx, a); };
    tools.push_back(std::move(edit));
    return tools;
}

}  // namespace wb::tools
