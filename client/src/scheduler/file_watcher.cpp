#include "scheduler/file_watcher.h"

#include "platform/win.h"

#include <windows.h>

#include <algorithm>
#include <cwctype>

namespace wb::scheduler {

namespace {

bool glob(const std::wstring& name, size_t n, const std::wstring& pat, size_t p) {
    while (p < pat.size()) {
        if (pat[p] == L'*') {
            for (size_t i = n; i <= name.size(); ++i)
                if (glob(name, i, pat, p + 1)) return true;
            return false;
        }
        if (n >= name.size()) return false;
        if (pat[p] != L'?' && std::towlower(pat[p]) != std::towlower(name[n])) return false;
        ++n;
        ++p;
    }
    return n == name.size();
}

long long filetime_ms(const FILETIME& f) {
    const unsigned long long t = (static_cast<unsigned long long>(f.dwHighDateTime) << 32) | f.dwLowDateTime;
    return static_cast<long long>(t / 10'000ULL) - 11'644'473'600'000LL;
}

bool same(const std::vector<FileStat>& a, const std::vector<FileStat>& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](const FileStat& x, const FileStat& y) {
               return x.path == y.path && x.size == y.size && x.mtime == y.mtime;
           });
}

}  // namespace

bool name_matches(const std::string& name, const std::string& patterns) {
    const std::wstring n = widen(name);
    size_t start = 0;
    const std::string ps = patterns.empty() ? "*" : patterns;
    while (start <= ps.size()) {
        size_t semi = ps.find(';', start);
        if (semi == std::string::npos) semi = ps.size();
        std::string p = ps.substr(start, semi - start);
        p.erase(0, p.find_first_not_of(' '));
        p.erase(p.find_last_not_of(' ') + 1);
        if (!p.empty() && glob(n, 0, widen(p), 0)) return true;
        start = semi + 1;
    }
    return false;
}

FileWatcher::FileWatcher(const Clock& clock, Report report) : clock_(clock), report_(std::move(report)) {}

FileWatcher::~FileWatcher() {
    for (auto& [_, w] : watches_) close(w);
}

void FileWatcher::close(Watch& w) {
    if (w.handle) ::FindCloseChangeNotification(static_cast<HANDLE>(w.handle));
    w.handle = nullptr;
}

void FileWatcher::set(const json& triggers) {
    std::map<std::string, Watch> next;
    if (triggers.is_array()) {
        for (const auto& t : triggers) {
            const std::string id = jstr(t, "id"), folder = jstr(t, "folder"), pattern = jstr(t, "pattern", "*");
            if (id.empty() || folder.empty()) continue;
            auto it = watches_.find(id);
            if (it != watches_.end() && it->second.folder == folder && it->second.pattern == pattern) {
                next[id] = std::move(it->second);
                it->second.handle = nullptr;
                continue;
            }
            Watch w;
            w.folder = folder;
            w.pattern = pattern;
            next[id] = std::move(w);
        }
    }
    for (auto& [_, w] : watches_) close(w);
    watches_ = std::move(next);
}

std::vector<FileStat> FileWatcher::list(const Watch& w) const {
    std::vector<FileStat> out;
    const std::wstring dir = widen(w.folder);
    WIN32_FIND_DATAW d{};
    HANDLE h = ::FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &d, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        const std::string name = narrow(d.cFileName);
        if (!name_matches(name, w.pattern)) continue;
        FileStat f;
        f.path = narrow(dir + L"\\" + d.cFileName);
        f.size = static_cast<long long>((static_cast<unsigned long long>(d.nFileSizeHigh) << 32) | d.nFileSizeLow);
        f.mtime = filetime_ms(d.ftLastWriteTime);
        out.push_back(std::move(f));
    } while (::FindNextFileW(h, &d));
    ::FindClose(h);
    std::sort(out.begin(), out.end(), [](const FileStat& a, const FileStat& b) { return a.path < b.path; });
    return out;
}

void FileWatcher::poll() {
    const long long now = clock_.unix_millis();
    for (auto& [id, w] : watches_) {
        if (!w.handle) {
            if (now < w.retry_at) continue;
            HANDLE h = ::FindFirstChangeNotificationW(widen(w.folder).c_str(), FALSE,
                                                      FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE);
            if (h == INVALID_HANDLE_VALUE) {
                w.retry_at = now + 60'000;  // the folder is missing or not readable now
                continue;
            }
            w.handle = h;
            w.dirty = true;
        }
        if (::WaitForSingleObject(static_cast<HANDLE>(w.handle), 0) == WAIT_OBJECT_0) {
            w.dirty = true;
            if (!::FindNextChangeNotification(static_cast<HANDLE>(w.handle))) close(w);
        }
        if (now - w.listed_at >= kFileRescanMs) w.dirty = true;
        // While a listing waits for its quiet period, the folder is listed again on every poll: some writes (a file
        // still growing) do not notify at once.
        if (!w.dirty && !w.pending) continue;
        auto files = list(w);
        w.listed_at = now;
        w.dirty = false;
        if (!w.pending || !same(files, w.listing)) {
            w.listing = std::move(files);
            w.pending = true;
            w.changed_at = now;
            continue;
        }
        if (now - w.changed_at < kFileQuietMs) continue;
        w.pending = false;
        report_(id, w.listing);
    }
}

}  // namespace wb::scheduler
