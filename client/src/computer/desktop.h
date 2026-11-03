// Windows, processes and the surfaces computer use cannot drive (client/computer-use Unreachable Surfaces Reported;
// add-computer-use design D3).
#pragma once

#include "base/result.h"
#include "computer/uia.h"

#include <windows.h>

#include <optional>
#include <string>
#include <vector>

namespace wb::computer {

struct WindowInfo {
    HWND hwnd = nullptr;
    std::string title;
    std::string process;  // executable file name, for example EXCEL.EXE
    DWORD pid = 0;
    Box box;              // the visible frame, physical pixels
    bool minimized = false;
};

// Visible top-level application windows with a title, front to back.
std::vector<WindowInfo> list_windows();
Result<WindowInfo> window_info(HWND hwnd);
// The executable file name of a process; empty when it cannot be read.
std::string process_name(DWORD pid);

// The surface that takes the input instead of the user's desktop: "the secure desktop (a UAC prompt)" or "the lock
// screen"; nullopt while the normal desktop has the input.
std::optional<std::string> unreachable_desktop();
// True when the process runs at a higher integrity level than this one (an elevated window, which Windows does not
// let a normal process drive), or when its level cannot be read.
bool runs_elevated(DWORD pid);

// Restores the window and makes it the foreground window; verifies it is (design D3).
Status bring_to_front(HWND hwnd);

}  // namespace wb::computer
