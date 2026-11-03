#pragma once

#include <windows.h>

#include <optional>
#include <string>

namespace wb {

std::optional<std::wstring> reg_read_string(HKEY root, const std::wstring& key, const std::wstring& name);
bool reg_write_string(HKEY root, const std::wstring& key, const std::wstring& name, const std::wstring& value);
bool reg_delete_value(HKEY root, const std::wstring& key, const std::wstring& name);
bool reg_delete_tree(HKEY root, const std::wstring& key);

}  // namespace wb
