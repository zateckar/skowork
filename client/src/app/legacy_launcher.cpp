// Compatibility entry point for shortcuts to the former executable name.
#include <windows.h>

#include <cwchar>

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    constexpr size_t capacity = 32768;
    wchar_t executable[capacity]{};
    const DWORD length = ::GetModuleFileNameW(nullptr, executable, static_cast<DWORD>(capacity));
    if (length == 0 || length >= capacity) return 1;
    size_t directory = length;
    while (directory > 0 && executable[directory - 1] != L'\\' && executable[directory - 1] != L'/') --directory;
    constexpr wchar_t name[] = L"Skowork.exe";
    constexpr size_t name_length = sizeof(name) / sizeof(name[0]) - 1;
    if (directory == 0 || directory + name_length >= capacity) return 1;
    std::wmemcpy(executable + directory, name, name_length + 1);

    // Keep the original argument tail intact, including quoting and backslashes.
    const wchar_t* tail = ::GetCommandLineW();
    if (*tail == L'"') {
        ++tail;
        while (*tail && *tail != L'"') ++tail;
        if (*tail) ++tail;
    } else {
        while (*tail && *tail != L' ' && *tail != L'\t') ++tail;
    }
    const size_t executable_length = directory + name_length;
    const size_t tail_length = std::wcslen(tail);
    if (executable_length + tail_length + 3 > capacity) return 1;
    wchar_t command[capacity]{};
    command[0] = L'"';
    std::wmemcpy(command + 1, executable, executable_length);
    command[executable_length + 1] = L'"';
    std::wmemcpy(command + executable_length + 2, tail, tail_length + 1);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION child{};
    if (!::CreateProcessW(executable, command, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &child)) {
        ::MessageBoxW(nullptr, L"Skowork could not be started. Check that Skowork.exe is installed next to this shortcut.",
                      L"Skowork", MB_OK | MB_ICONERROR);
        return 1;
    }
    ::CloseHandle(child.hThread);
    ::CloseHandle(child.hProcess);
    return 0;
}
