// Test helper: a directory junction (what mklink /J makes), created natively. Junctions need no privilege.
#pragma once

#include "platform/win.h"

#include <windows.h>
#include <winioctl.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace wb::test {

// Makes `link` (a new, empty directory) a mount-point reparse point to `final_target`, which must be a final path
// (policy::canonicalize). Remove it with RemoveDirectoryW before removing the folder that holds it.
inline bool make_junction(const std::filesystem::path& link, const std::wstring& final_target) {
    if (!std::filesystem::create_directory(link)) return false;
    unique_file h(::CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                                nullptr));
    if (!h) return false;
    const std::wstring sub = L"\\??\\" + final_target, print = final_target;
    const auto sub_bytes = static_cast<USHORT>(sub.size() * sizeof(wchar_t));
    const auto print_bytes = static_cast<USHORT>(print.size() * sizeof(wchar_t));
    // REPARSE_DATA_BUFFER (MountPointReparseBuffer): tag, data length, reserved, then four offsets/lengths and the paths.
    std::vector<std::uint8_t> buf(8 + 8 + sub_bytes + 2 + print_bytes + 2, 0);
    const auto put16 = [&](size_t at, USHORT v) { std::memcpy(buf.data() + at, &v, 2); };
    const ULONG tag = IO_REPARSE_TAG_MOUNT_POINT;
    std::memcpy(buf.data(), &tag, 4);
    put16(4, static_cast<USHORT>(buf.size() - 8));
    put16(8, 0);
    put16(10, sub_bytes);
    put16(12, static_cast<USHORT>(sub_bytes + 2));
    put16(14, print_bytes);
    std::memcpy(buf.data() + 16, sub.data(), sub_bytes);
    std::memcpy(buf.data() + 16 + sub_bytes + 2, print.data(), print_bytes);
    DWORD n = 0;
    return ::DeviceIoControl(h.get(), FSCTL_SET_REPARSE_POINT, buf.data(), static_cast<DWORD>(buf.size()), nullptr, 0, &n, nullptr) != 0;
}

}  // namespace wb::test
