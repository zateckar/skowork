#include "policy/paths.h"

#include <winternl.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <deque>
#include <vector>

namespace wb::policy {

namespace {

// NtCreateFile values (ntifs.h); named here so they cannot collide with SDK macros.
constexpr ULONG kFileCreate = 2;
constexpr ULONG kFileOpenIf = 3;
constexpr ULONG kFileNonDirectoryFile = 0x00000040;
constexpr ULONG kFileSynchronousIoNonalert = 0x00000020;
constexpr ULONG kFileOpenReparsePoint = 0x00200000;
constexpr ULONG kObjCaseInsensitive = 0x00000040;

using NtCreateFileFn = LONG(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK, PLARGE_INTEGER, ULONG, ULONG, ULONG,
                                     ULONG, PVOID, ULONG);

NtCreateFileFn nt_create_file() {
    static const auto fn = reinterpret_cast<NtCreateFileFn>(
        reinterpret_cast<void*>(::GetProcAddress(::GetModuleHandleW(L"ntdll.dll"), "NtCreateFile")));
    return fn;
}

Error invalid(const std::string& why) { return make_error("policy_denied", why); }

bool is_missing(DWORD e) { return e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND; }

Result<std::wstring> full_path(std::string_view path) {
    const std::string expanded = expand_environment(path);
    if (auto p = path_syntax_problem(expanded)) return invalid(*p);
    const std::wstring w = widen(expanded);
    DWORD n = ::GetFullPathNameW(w.c_str(), 0, nullptr, nullptr);
    if (n == 0) return make_error("io", "GetFullPathName: " + error_text(::GetLastError()));
    std::wstring full(n, L'\0');
    n = ::GetFullPathNameW(w.c_str(), n, full.data(), nullptr);
    full.resize(n);
    // GetFullPathName maps names such as "C:\x\CON" to device paths: check again.
    if (auto p = path_syntax_problem(narrow(full))) return invalid(*p);
    return full;
}

// Splits "C:\a\b" into "C:\a" and "b"; returns false for a root.
bool split_last(const std::wstring& p, std::wstring& parent, std::wstring& name) {
    size_t end = p.size();
    while (end > 0 && (p[end - 1] == L'\\' || p[end - 1] == L'/')) --end;
    const size_t slash = p.find_last_of(L"\\/", end == 0 ? 0 : end - 1);
    if (slash == std::wstring::npos || end == 0) return false;
    name = p.substr(slash + 1, end - slash - 1);
    parent = p.substr(0, slash);
    if (parent.size() == 2 && parent[1] == L':') parent += L'\\';
    if (parent.empty() || parent == L"\\" || (parent.size() <= 2 && parent[0] == L'\\')) return false;  // UNC server root
    return !name.empty();
}

unique_file open_existing(const std::wstring& p, DWORD access) {
    return unique_file(::CreateFileW(p.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                     FILE_FLAG_BACKUP_SEMANTICS, nullptr));
}

std::string join(std::string base, const std::deque<std::wstring>& tail) {
    for (const auto& t : tail) {
        if (base.empty() || base.back() != '\\') base += '\\';
        base += narrow(t);
    }
    return base;
}

}  // namespace

std::string expand_environment(std::string_view s, const std::vector<std::pair<std::string, std::string>>& extra) {
    std::string text(s);
    for (const auto& [name, value] : extra) {
        const std::string token = "%" + name + "%";
        for (size_t at = 0;;) {
            // Case-insensitive like Windows environment names.
            size_t found = std::string::npos;
            for (size_t i = at; i + token.size() <= text.size(); ++i) {
                if (_strnicmp(text.c_str() + i, token.c_str(), token.size()) == 0) {
                    found = i;
                    break;
                }
            }
            if (found == std::string::npos) break;
            text.replace(found, token.size(), value);
            at = found + value.size();
        }
    }
    if (text.find('%') == std::string::npos) return text;
    const std::wstring w = widen(text);
    const DWORD n = ::ExpandEnvironmentStringsW(w.c_str(), nullptr, 0);
    if (n == 0) return text;
    std::wstring out(n, L'\0');
    const DWORD m = ::ExpandEnvironmentStringsW(w.c_str(), out.data(), n);
    if (m == 0 || m > n) return text;
    out.resize(m - 1);
    return narrow(out);
}

Result<std::string> final_path_of(HANDLE h) {
    std::wstring buf(512, L'\0');
    for (int i = 0; i < 3; ++i) {
        const DWORD n = ::GetFinalPathNameByHandleW(h, buf.data(), static_cast<DWORD>(buf.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        if (n == 0) return make_error("io", "GetFinalPathNameByHandle: " + error_text(::GetLastError()));
        if (n < buf.size()) {
            buf.resize(n);
            return normalize_final_path(narrow(buf));
        }
        buf.resize(n + 1);
    }
    return make_error("io", "final path keeps growing");
}

Result<std::string> canonicalize(std::string_view path) {
    auto full = full_path(path);
    if (!full) return full.error();
    std::wstring cur = *full;
    std::deque<std::wstring> tail;
    for (int depth = 0; depth < 512; ++depth) {
        unique_file h = open_existing(cur, FILE_READ_ATTRIBUTES);
        if (h) {
            auto fp = final_path_of(h.get());
            if (!fp) return fp.error();
            std::string out = join(*fp, tail);
            if (auto p = path_syntax_problem(out)) return invalid(*p);
            return out;
        }
        const DWORD e = ::GetLastError();
        if (!is_missing(e)) return make_error("io", "cannot resolve " + narrow(cur) + ": " + error_text(e));
        std::wstring parent, name;
        if (!split_last(cur, parent, name)) return make_error("io", "no existing folder in " + narrow(*full));
        tail.push_front(name);
        cur = parent;
    }
    return make_error("io", "path too deep");
}

Result<unique_file> open_for_read(std::string_view path, const PathRules& rules, RuleVerdict* verdict) {
    auto fp = canonicalize(path);
    if (!fp) return fp.error();
    RuleVerdict v = check_path(*fp, Access::read, rules);
    if (verdict) *verdict = v;
    if (!v.allowed) return make_error("policy_denied", v.rule + ": " + v.reason);
    unique_file h = open_existing(widen(*fp), GENERIC_READ);
    if (!h) return make_error("io", "cannot open " + *fp + ": " + error_text(::GetLastError()));
    // Check what was actually opened: a link swapped in after the check is caught here.
    auto opened = final_path_of(h.get());
    if (!opened) return opened.error();
    v = check_path(*opened, Access::read, rules);
    if (verdict) *verdict = v;
    if (!v.allowed) return make_error("policy_denied", v.rule + ": " + v.reason);
    return h;
}

namespace {

// The opened, checked parent folder of a file to write, and the target's final path.
struct CheckedParent {
    unique_file dir;
    std::wstring name;
    std::string target;
};

Result<CheckedParent> open_checked_parent(std::string_view path, const PathRules& rules, RuleVerdict* verdict) {
    auto full = full_path(path);
    if (!full) return full.error();
    CheckedParent p;
    std::wstring parent;
    if (!split_last(*full, parent, p.name) || p.name == L"." || p.name == L".." || p.name.find(L':') != std::wstring::npos)
        return invalid("not a file path");
    // The parent folder is opened first; its final path is fixed from now on.
    p.dir = unique_file(::CreateFileW(parent.c_str(), FILE_LIST_DIRECTORY | FILE_ADD_FILE | FILE_TRAVERSE | SYNCHRONIZE,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                      FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    if (!p.dir) return make_error("io", "cannot open the folder " + narrow(parent) + ": " + error_text(::GetLastError()));
    auto dir_path = final_path_of(p.dir.get());
    if (!dir_path) return dir_path.error();
    p.target = *dir_path;
    if (p.target.back() != '\\') p.target += '\\';
    p.target += narrow(p.name);
    const RuleVerdict v = check_path(p.target, Access::write, rules);
    if (verdict) *verdict = v;
    if (!v.allowed) return make_error("policy_denied", v.rule + ": " + v.reason);
    return p;
}

std::string ntstatus_text(LONG st) {
    char b[16];
    std::snprintf(b, sizeof b, "%08lX", static_cast<unsigned long>(st));
    return std::string("NTSTATUS 0x") + b;
}

// Opens or creates `name` relative to the open folder `dir`, never following a link in its place.
LONG open_relative(HANDLE dir, const std::wstring& name, ACCESS_MASK access, ULONG share, ULONG disposition, HANDLE* out) {
    const auto create = nt_create_file();
    if (!create) return static_cast<LONG>(0xC0000002L);  // STATUS_NOT_IMPLEMENTED
    std::wstring copy = name;
    UNICODE_STRING us;
    us.Buffer = copy.data();
    us.Length = static_cast<USHORT>(copy.size() * sizeof(wchar_t));
    us.MaximumLength = us.Length;
    OBJECT_ATTRIBUTES oa;
    InitializeObjectAttributes(&oa, &us, kObjCaseInsensitive, dir, nullptr);
    IO_STATUS_BLOCK iosb{};
    return create(out, access | SYNCHRONIZE, &oa, &iosb, nullptr, FILE_ATTRIBUTE_NORMAL, share, disposition,
                  kFileNonDirectoryFile | kFileSynchronousIoNonalert | kFileOpenReparsePoint, nullptr, 0);
}

// Refuses links: a reparse point in the file's place, or a hard link shared with another path.
Status refuse_links(HANDLE h, const std::string& target, RuleVerdict* verdict) {
    FILE_ATTRIBUTE_TAG_INFO tag{};
    if (!::GetFileInformationByHandleEx(h, FileAttributeTagInfo, &tag, sizeof tag)) return make_error("io", "cannot inspect " + target);
    if (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
        if (verdict) *verdict = {false, "link", "writing through a link is not allowed"};
        return make_error("policy_denied", "link: writing through a link is not allowed");
    }
    BY_HANDLE_FILE_INFORMATION info{};
    if (!::GetFileInformationByHandle(h, &info)) return make_error("io", "cannot inspect " + target);
    if (info.nNumberOfLinks > 1) {
        if (verdict) *verdict = {false, "link", "the file has other hard links"};
        return make_error("policy_denied", "link: the file has other hard links");
    }
    return {};
}

}  // namespace

Result<unique_file> open_for_write(std::string_view path, WriteMode mode, const PathRules& rules, RuleVerdict* verdict) {
    auto parent = open_checked_parent(path, rules, verdict);
    if (!parent) return parent.error();
    const std::string& target = parent->target;
    HANDLE raw = nullptr;
    const LONG st = open_relative(parent->dir.get(), parent->name, FILE_GENERIC_WRITE | FILE_GENERIC_READ, FILE_SHARE_READ,
                                  mode == WriteMode::create_new ? kFileCreate : kFileOpenIf, &raw);
    if (st < 0) return make_error("io", "cannot create " + target + ": " + ntstatus_text(st));
    unique_file h(raw);
    if (Status s = refuse_links(h.get(), target, verdict); !s) return s.error();
    RuleVerdict v;
    auto opened = final_path_of(h.get());
    if (!opened) return opened.error();
    v = check_path(*opened, Access::write, rules);
    if (verdict) *verdict = v;
    if (!v.allowed) return make_error("policy_denied", v.rule + ": " + v.reason);
    if (mode == WriteMode::truncate_or_create) {
        FILE_END_OF_FILE_INFO eof{};
        if (!::SetFileInformationByHandle(h.get(), FileEndOfFileInfo, &eof, sizeof eof))
            return make_error("io", "cannot truncate " + target + ": " + error_text(::GetLastError()));
    }
    return h;
}

Result<std::string> write_file_atomic(std::string_view path, std::string_view content, const PathRules& rules, RuleVerdict* verdict) {
    auto parent = open_checked_parent(path, rules, verdict);
    if (!parent) return parent.error();
    const std::string& target = parent->target;
    HANDLE dir = parent->dir.get();

    // The file being replaced, if any, must not be a link.
    {
        HANDLE raw = nullptr;
        const LONG st = open_relative(dir, parent->name, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                      1 /* FILE_OPEN */, &raw);
        if (st >= 0) {
            unique_file existing(raw);
            if (Status s = refuse_links(existing.get(), target, verdict); !s) return s.error();
        }
    }

    // The new content goes to a temporary file next to the target.
    wchar_t suffix[32];
    std::swprintf(suffix, 32, L".%08lx%08lx.tmp", ::GetCurrentProcessId(), ::GetTickCount());
    const std::wstring temp_name = L"." + parent->name + suffix;
    HANDLE raw = nullptr;
    LONG st = open_relative(dir, temp_name, FILE_GENERIC_WRITE | FILE_GENERIC_READ | DELETE, 0, kFileCreate, &raw);
    if (st < 0) return make_error("io", "cannot create a temporary file next to " + target + ": " + ntstatus_text(st));
    unique_file temp(raw);
    auto discard = [&] {
        FILE_DISPOSITION_INFO del{TRUE};
        ::SetFileInformationByHandle(temp.get(), FileDispositionInfo, &del, sizeof del);
    };
    for (size_t off = 0; off < content.size();) {
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(content.size() - off, 1u << 20));
        DWORD written = 0;
        if (!::WriteFile(temp.get(), content.data() + off, chunk, &written, nullptr) || written == 0) {
            const DWORD e = ::GetLastError();
            discard();
            return make_error("io", "cannot write " + target + ": " + error_text(e));
        }
        off += written;
    }
    ::FlushFileBuffers(temp.get());

    // Rename over the target, relative to the checked folder handle (never through a path that could be swapped).
    // NtSetInformationFile, because the kernel32 wrapper refuses a RootDirectory.
    struct RenameInfo {
        BOOLEAN ReplaceIfExists;
        HANDLE RootDirectory;
        ULONG FileNameLength;
        WCHAR FileName[1];
    };
    using NtSetInformationFileFn = LONG(NTAPI*)(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG, ULONG);
    static const auto set_info = reinterpret_cast<NtSetInformationFileFn>(
        reinterpret_cast<void*>(::GetProcAddress(::GetModuleHandleW(L"ntdll.dll"), "NtSetInformationFile")));
    std::vector<unsigned char> buf(offsetof(RenameInfo, FileName) + parent->name.size() * sizeof(wchar_t) + sizeof(wchar_t));
    auto* ri = reinterpret_cast<RenameInfo*>(buf.data());
    ri->ReplaceIfExists = TRUE;
    ri->RootDirectory = dir;
    ri->FileNameLength = static_cast<ULONG>(parent->name.size() * sizeof(wchar_t));
    std::memcpy(ri->FileName, parent->name.data(), ri->FileNameLength);
    IO_STATUS_BLOCK iosb{};
    st = set_info ? set_info(temp.get(), &iosb, ri, static_cast<ULONG>(buf.size()), 10 /* FileRenameInformation */) : -1;
    if (st < 0) {
        discard();
        return make_error("io", "cannot replace " + target + ": " + ntstatus_text(st));
    }
    auto written = final_path_of(temp.get());
    if (!written) return written.error();
    return *written;
}

PathRules path_rules(const EffectivePolicy* policy, const std::string& workspace, const std::string& data_root,
                     std::vector<FolderGrant> grants) {
    PathRules r;
    // Final paths, like the paths they are compared with: the profile may be redirected or reached through a link.
    const auto final_or_given = [](const std::string& p) {
        auto c = canonicalize(p);
        return c ? *c : normalize_final_path(p);
    };
    r.workspace = final_or_given(workspace);
    r.data_root = final_or_given(data_root);
    r.grants = std::move(grants);
    if (!policy) return r;
    const std::vector<std::pair<std::string, std::string>> extra = {{"WORKBENCH_DATA", r.data_root}};
    auto load = [&](const char* key, std::vector<std::string>& out) {
        for (const auto& g : policy->strings(key)) {
            std::string e = expand_environment(g, extra);
            for (auto& c : e)
                if (c == '/') c = '\\';
            out.push_back(std::move(e));
        }
    };
    load("filesystem.read", r.read);
    load("filesystem.write", r.write);
    load("filesystem.deny", r.deny);
    return r;
}

}  // namespace wb::policy
