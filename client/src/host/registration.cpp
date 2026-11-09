#include "host/registration.h"

#include "platform/registry.h"

#include <windows.h>

namespace wb {

std::wstring run_command(const std::filesystem::path& exe) { return L"\"" + exe.wstring() + L"\" --background"; }

RegistrationOutcome ensure_registration(const std::filesystem::path& exe, const RegistrationPaths& p) {
    RegistrationOutcome out;
    const std::wstring run = run_command(exe);
    if (reg_read_string(HKEY_CURRENT_USER, p.run_key, p.run_value) != run) {
        out.run_repaired = true;
        out.ok &= reg_write_string(HKEY_CURRENT_USER, p.run_key, p.run_value, run);
    }
    if (reg_read_string(HKEY_CURRENT_USER, p.aumid_key, L"DisplayName") != L"SkoWork")
        out.ok &= reg_write_string(HKEY_CURRENT_USER, p.aumid_key, L"DisplayName", L"SkoWork");
    return out;
}

}  // namespace wb
