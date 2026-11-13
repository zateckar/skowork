// Per-user registration (client/desktop-host Per-User Registration): start at logon under HKCU, repaired at every
// start. Sign-in uses a loopback redirect (client/identity), so no URI scheme is registered. The AUMID of the toast
// notifications (add-work-inbox design D6) is registered under HKCU\Software\Classes\AppUserModelId.
#pragma once

#include <filesystem>
#include <string>

namespace wb {

inline constexpr wchar_t kAppUserModelId[] = L"Skowork.Client";

struct RegistrationPaths {
    std::wstring run_key = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    std::wstring run_value = L"Skowork";
    std::wstring aumid_key = std::wstring(L"Software\\Classes\\AppUserModelId\\") + kAppUserModelId;
};

struct RegistrationOutcome {
    bool run_repaired = false;
    bool ok = true;
};

std::wstring run_command(const std::filesystem::path& exe);

// Writes the entries when they are missing or point to another executable.
RegistrationOutcome ensure_registration(const std::filesystem::path& exe, const RegistrationPaths& paths = {});

}  // namespace wb
