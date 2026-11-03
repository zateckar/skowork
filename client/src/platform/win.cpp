#include "platform/win.h"

#include <aclapi.h>
#include <sddl.h>
#define SECURITY_WIN32
#include <secext.h>
#include <shlobj.h>

#include <cstdio>
#include <vector>

namespace wb {

std::wstring widen(std::string_view utf8) {
    if (utf8.empty()) return {};
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), n);
    return out;
}

std::string narrow(std::wstring_view wide) {
    if (wide.empty()) return {};
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr,
                                        nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::string error_text(DWORD code) {
    wchar_t* msg = nullptr;
    HMODULE winhttp = ::GetModuleHandleW(L"winhttp.dll");
    DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
    if (winhttp && code >= 12000 && code < 12200) flags |= FORMAT_MESSAGE_FROM_HMODULE;
    const DWORD len = ::FormatMessageW(flags, winhttp, code, 0, reinterpret_cast<wchar_t*>(&msg), 0, nullptr);
    std::wstring text = len ? std::wstring(msg, len) : L"";
    if (msg) ::LocalFree(msg);
    while (!text.empty() && (text.back() == L'\n' || text.back() == L'\r' || text.back() == L' ')) text.pop_back();
    char hex[16];
    std::snprintf(hex, sizeof hex, "0x%08lX", static_cast<unsigned long>(code));
    return std::string(hex) + (text.empty() ? "" : " (" + narrow(text) + ")");
}

std::wstring current_user_sid() {
    unique_handle token;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, token.put())) return {};
    DWORD size = 0;
    ::GetTokenInformation(token.get(), TokenUser, nullptr, 0, &size);
    std::vector<std::byte> buf(size);
    if (!::GetTokenInformation(token.get(), TokenUser, buf.data(), size, &size)) return {};
    wchar_t* str = nullptr;
    if (!::ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buf.data())->User.Sid, &str)) return {};
    std::wstring sid(str);
    ::LocalFree(str);
    return sid;
}

std::wstring computer_name() {
    wchar_t buf[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD n = MAX_COMPUTERNAME_LENGTH + 1;
    return ::GetComputerNameW(buf, &n) ? std::wstring(buf, n) : L"?";
}

std::wstring user_name() {
    wchar_t buf[257];
    DWORD n = 257;
    return ::GetUserNameW(buf, &n) ? std::wstring(buf) : L"?";
}

std::wstring user_principal() {
    wchar_t buf[512];
    ULONG n = 512;
    if (::GetUserNameExW(NameUserPrincipal, buf, &n)) return std::wstring(buf, n);
    n = 512;
    if (::GetUserNameExW(NameSamCompatible, buf, &n)) return std::wstring(buf, n);
    return user_name();
}

std::wstring local_appdata() {
    wchar_t* p = nullptr;
    std::wstring out;
    if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &p))) out = p;
    ::CoTaskMemFree(p);
    return out;
}

std::filesystem::path exe_path() {
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = ::GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (n < buf.size()) {
            buf.resize(n);
            return buf;
        }
        buf.resize(buf.size() * 2);
    }
}

std::filesystem::path exe_dir() { return exe_path().parent_path(); }

std::string os_build() {
    // RtlGetVersion is not affected by manifest-based version lies.
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    OSVERSIONINFOW vi{};
    vi.dwOSVersionInfoSize = sizeof vi;
    if (HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll"))
        if (auto fn = reinterpret_cast<RtlGetVersionFn>(reinterpret_cast<void*>(::GetProcAddress(ntdll, "RtlGetVersion"))))
            fn(&vi);
    DWORD ubr = 0, size = sizeof ubr;
    ::RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"UBR", RRF_RT_REG_DWORD,
                   nullptr, &ubr, &size);
    char buf[64];
    std::snprintf(buf, sizeof buf, "%lu.%lu.%lu.%lu", static_cast<unsigned long>(vi.dwMajorVersion),
                  static_cast<unsigned long>(vi.dwMinorVersion), static_cast<unsigned long>(vi.dwBuildNumber),
                  static_cast<unsigned long>(ubr));
    return buf;
}

std::string hardware_class() {
    SYSTEM_INFO si{};
    ::GetNativeSystemInfo(&si);
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof ms;
    ::GlobalMemoryStatusEx(&ms);
    const unsigned long long gb = (ms.ullTotalPhys + (512ULL << 20)) >> 30;
    return "cpu:" + std::to_string(si.dwNumberOfProcessors) + "c;ram:" + std::to_string(gb) + "GB";
}

bool create_private_directory(const std::wstring& path, std::string& err) {
    const std::wstring sid = current_user_sid();
    if (sid.empty()) {
        err = "cannot read user SID";
        return false;
    }
    // P = protected (no inheritance from the parent); OICI = inherited by children.
    const std::wstring sddl = L"D:P(A;OICI;FA;;;" + sid + L")(A;OICI;FA;;;SY)";
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!::ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &sd, nullptr)) {
        err = "SDDL: " + error_text(::GetLastError());
        return false;
    }
    unique_hlocal sd_guard(sd);
    SECURITY_ATTRIBUTES sa{sizeof sa, sd, FALSE};
    const int rc = ::SHCreateDirectoryExW(nullptr, path.c_str(), &sa);
    if (rc == ERROR_ALREADY_EXISTS || rc == ERROR_FILE_EXISTS) {
        if (!::SetFileSecurityW(path.c_str(), DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, sd)) {
            err = "SetFileSecurity: " + error_text(::GetLastError());
            return false;
        }
        return true;
    }
    if (rc != ERROR_SUCCESS) {
        err = "create directory: " + error_text(static_cast<DWORD>(rc));
        return false;
    }
    return true;
}

bool is_private_directory(const std::wstring& path, std::string& detail) {
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (::GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &dacl,
                                nullptr, &sd) != ERROR_SUCCESS) {
        detail = "cannot read DACL";
        return false;
    }
    unique_hlocal guard(sd);
    SECURITY_DESCRIPTOR_CONTROL control = 0;
    DWORD rev = 0;
    ::GetSecurityDescriptorControl(sd, &control, &rev);
    if (!(control & SE_DACL_PROTECTED)) {
        detail = "DACL inherits from parent";
        return false;
    }
    const std::wstring me = current_user_sid();
    ACL_SIZE_INFORMATION info{};
    if (!dacl || !::GetAclInformation(dacl, &info, sizeof info, AclSizeInformation)) {
        detail = "no DACL";
        return false;
    }
    for (DWORD i = 0; i < info.AceCount; ++i) {
        void* ace = nullptr;
        if (!::GetAce(dacl, i, &ace)) continue;
        const auto* header = static_cast<ACE_HEADER*>(ace);
        if (header->AceType != ACCESS_ALLOWED_ACE_TYPE) continue;
        PSID sid = &static_cast<ACCESS_ALLOWED_ACE*>(ace)->SidStart;
        wchar_t* str = nullptr;
        if (!::ConvertSidToStringSidW(sid, &str)) continue;
        const std::wstring s(str);
        ::LocalFree(str);
        if (s != me && s != L"S-1-5-18") {
            detail = "access granted to " + narrow(s);
            return false;
        }
    }
    return true;
}

}  // namespace wb
