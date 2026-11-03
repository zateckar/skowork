#include "platform/registry.h"

#include "platform/win.h"

namespace wb {

std::optional<std::wstring> reg_read_string(HKEY root, const std::wstring& key, const std::wstring& name) {
    DWORD size = 0;
    if (::RegGetValueW(root, key.c_str(), name.empty() ? nullptr : name.c_str(), RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ,
                       nullptr, nullptr, &size) != ERROR_SUCCESS)
        return std::nullopt;
    std::wstring value(size / sizeof(wchar_t), L'\0');
    if (::RegGetValueW(root, key.c_str(), name.empty() ? nullptr : name.c_str(), RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ,
                       nullptr, value.data(), &size) != ERROR_SUCCESS)
        return std::nullopt;
    value.resize(size / sizeof(wchar_t));
    while (!value.empty() && value.back() == L'\0') value.pop_back();
    return value;
}

bool reg_write_string(HKEY root, const std::wstring& key, const std::wstring& name, const std::wstring& value) {
    unique_hkey k;
    if (::RegCreateKeyExW(root, key.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, k.put(), nullptr) != ERROR_SUCCESS)
        return false;
    return ::RegSetValueExW(k.get(), name.empty() ? nullptr : name.c_str(), 0, REG_SZ,
                            reinterpret_cast<const BYTE*>(value.c_str()),
                            static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
}

bool reg_delete_value(HKEY root, const std::wstring& key, const std::wstring& name) {
    const LSTATUS rc = ::RegDeleteKeyValueW(root, key.c_str(), name.c_str());
    return rc == ERROR_SUCCESS || rc == ERROR_FILE_NOT_FOUND;
}

bool reg_delete_tree(HKEY root, const std::wstring& key) {
    const LSTATUS rc = ::RegDeleteTreeW(root, key.c_str());
    if (rc != ERROR_SUCCESS && rc != ERROR_FILE_NOT_FOUND) return false;
    const LSTATUS rc2 = ::RegDeleteKeyW(root, key.c_str());
    return rc2 == ERROR_SUCCESS || rc2 == ERROR_FILE_NOT_FOUND;
}

}  // namespace wb
