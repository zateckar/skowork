// Workbench.exe: the per-user resident process (client/desktop-host). Starts the tray, storage, identity and the
// control channel; the UI window is created only on demand.
#include "app/app.h"

#include <windows.h>
#include <shellapi.h>

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    int argc = 0;
    wchar_t** argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
    std::vector<std::wstring> args;
    for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);
    ::LocalFree(argv);
    return wb::run_app(inst, args);
}
