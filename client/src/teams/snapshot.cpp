// Teams cache snapshot and profile discovery (client/teams; add-local-outlook-and-teams design D4).
#include "teams/snapshot.h"

#include <algorithm>
#include <fstream>
#include <optional>
#include <set>
#include <string>
#include <system_error>
#include <utility>

#include "teams/leveldb.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace wb::teams {
namespace {

namespace fs = std::filesystem;

constexpr std::uint64_t kMaxCopySize = std::uint64_t{1} << 30;

// Reads a whole source file without blocking its writer. "vanished" when it does not exist (any more).
Status read_shared(const fs::path& path, std::string& out) {
    out.clear();
#ifdef _WIN32
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD err = GetLastError();
        if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) return make_error("vanished", "file disappeared");
        return make_error("io", "cannot open a cache file");
    }
    char buf[1 << 16];
    for (;;) {
        DWORD got = 0;
        if (!ReadFile(h, buf, sizeof buf, &got, nullptr)) {
            CloseHandle(h);
            return make_error("io", "cannot read a cache file");
        }
        if (got == 0) break;
        out.append(buf, got);
        if (out.size() > kMaxCopySize) {
            CloseHandle(h);
            return make_error("io", "cache file too large");
        }
    }
    CloseHandle(h);
    return {};
#else
    std::error_code ec;
    if (!fs::exists(path, ec)) return make_error("vanished", "file disappeared");
    std::ifstream f(path, std::ios::binary);
    if (!f) return make_error(fs::exists(path, ec) ? "io" : "vanished", "cannot open a cache file");
    char buf[1 << 16];
    while (f.read(buf, sizeof buf) || f.gcount() > 0) {
        out.append(buf, static_cast<std::size_t>(f.gcount()));
        if (out.size() > kMaxCopySize) return make_error("io", "cache file too large");
    }
    return {};
#endif
}

Status write_private(const fs::path& path, const std::string& data) {
    fs::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return make_error("io", "cannot write the snapshot");
        f.write(data.data(), static_cast<std::streamsize>(data.size()));
        if (!f) return make_error("io", "cannot write the snapshot");
    }
    std::error_code ec;
    fs::remove(path, ec);
    fs::rename(tmp, path, ec);
    if (ec) return make_error("io", "cannot place the snapshot file");
    return {};
}

Status copy_file_shared(const fs::path& from, const fs::path& to) {
    std::string data;
    if (Status s = read_shared(from, data); !s) return s;
    return write_private(to, data);
}

bool has_ext(const std::string& name, const char* ext) {
    const std::string e(ext);
    return name.size() > e.size() && name.compare(name.size() - e.size(), e.size(), e) == 0;
}

Status attempt(const fs::path& src, const fs::path& dst) {
    std::string current;
    if (Status s = read_shared(src / "CURRENT", current); !s)
        return s.error().code == "vanished" ? make_error("not_found", "no CURRENT file") : s;
    const std::string manifest_name = manifest_name_from_current(current);
    if (manifest_name.empty()) return make_error("corrupt", "CURRENT does not name a manifest");
    std::string manifest;
    if (Status s = read_shared(src / manifest_name, manifest); !s) return s;  // vanished: CURRENT moved on, retry
    const auto parsed = parse_manifest(manifest);

    std::set<std::string> tables_present, logs_present;
    std::error_code ec;
    for (fs::directory_iterator it(src, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (has_ext(name, ".ldb") || has_ext(name, ".sst")) tables_present.insert(name);
        else if (has_ext(name, ".log")) logs_present.insert(name);
    }
    if (ec) return make_error("io", "cannot list the cache folder");

    std::set<std::string> tables;
    if (parsed) {
        for (const auto n : parsed->live_tables) {
            std::string digits = std::to_string(n);
            if (digits.size() < 6) digits.insert(0, 6 - digits.size(), '0');
            if (tables_present.count(digits + ".ldb")) tables.insert(digits + ".ldb");
            else if (tables_present.count(digits + ".sst")) tables.insert(digits + ".sst");
            else return make_error("vanished", "a live table is missing");  // compaction raced the listing
        }
    } else {
        tables = tables_present;
    }

    for (const auto& name : tables) {
        const auto src_size = fs::file_size(src / name, ec);
        if (ec) return make_error("vanished", "table disappeared");
        std::error_code dec;
        const auto dst_size = fs::file_size(dst / name, dec);
        if (!dec && dst_size == src_size) continue;  // tables are immutable: same name and size is the same file
        if (Status s = copy_file_shared(src / name, dst / name); !s) return s;
    }
    for (const auto& name : logs_present)
        if (Status s = copy_file_shared(src / name, dst / name); !s) return s;
    if (Status s = write_private(dst / manifest_name, manifest); !s) return s;
    if (Status s = write_private(dst / "CURRENT", current); !s) return s;

    std::set<std::string> keep = tables;
    keep.insert(logs_present.begin(), logs_present.end());
    keep.insert(manifest_name);
    keep.insert("CURRENT");
    std::vector<fs::path> stale;
    for (fs::directory_iterator it(dst, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code tec;
        if (it->is_regular_file(tec) && !keep.count(it->path().filename().string())) stale.push_back(it->path());
    }
    for (const auto& p : stale) fs::remove(p, ec);
    return {};
}

}  // namespace

Result<std::filesystem::path> snapshot_cache(const std::filesystem::path& leveldb_dir,
                                             const std::filesystem::path& private_dir) {
    std::error_code ec;
    if (!fs::is_directory(leveldb_dir, ec)) return make_error("not_found", "cache folder missing");
    fs::create_directories(private_dir, ec);
    if (!fs::is_directory(private_dir, ec)) return make_error("io", "cannot create the snapshot folder");
    Status s = attempt(leveldb_dir, private_dir);
    if (!s && s.error().code == "vanished") s = attempt(leveldb_dir, private_dir);
    if (!s) return s.error();
    return private_dir;
}

std::vector<std::filesystem::path> find_teams_profiles(const std::filesystem::path& local_app_data) {
    const fs::path base =
        local_app_data / "Packages" / "MSTeams_8wekyb3d8bbwe" / "LocalCache" / "Microsoft" / "MSTeams" / "EBWebView";
    std::vector<std::pair<fs::file_time_type, fs::path>> found;
    std::error_code ec;
    for (fs::directory_iterator it(base, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (name.rfind("WV2Profile_", 0) != 0) continue;
        const fs::path db = it->path() / "IndexedDB" / "https_teams.microsoft.com_0.indexeddb.leveldb";
        std::error_code dec;
        if (!fs::is_directory(db, dec)) continue;
        auto t = fs::last_write_time(db, dec);
        if (dec) t = fs::file_time_type::min();
        found.emplace_back(t, db);
    }
    std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    std::vector<fs::path> out;
    for (auto& [t, p] : found) out.push_back(std::move(p));
    return out;
}

}  // namespace wb::teams
