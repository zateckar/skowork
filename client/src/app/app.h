#pragma once

#include <windows.h>

#include <optional>
#include <string>
#include <vector>

namespace wb {

struct LaunchArgs {
    bool background = false;                // started at logon: no window
    std::string route;                      // SPA route to open, e.g. "#/about"
    // Development builds only (WORKBENCH_DEV_ENDPOINTS): isolate a test instance.
    std::optional<std::wstring> data_root;
    std::optional<std::wstring> pipe_name;
    std::optional<std::wstring> config_file;
    bool no_register = false;  // skip HKCU registration (tests)
};

LaunchArgs parse_launch_args(const std::vector<std::wstring>& args);

int run_app(HINSTANCE inst, const std::vector<std::wstring>& args);

}  // namespace wb
