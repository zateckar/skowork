// Runs Skowork's own remote-track capture (AudioCapture, process source) on the outermost ms-teams.exe for N seconds
// and writes an encrypted track, exactly as a meeting does (fix-teams-remote-audio acceptance). Run during a call:
//   capture_probe.exe <out.skau> [seconds]
#include "meeting/audio_file.h"
#include "meeting/capture.h"
#include "platform/log.h"

#include <windows.h>
#include <tlhelp32.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <thread>

int main(int argc, char** argv) {
    if (argc < 2) { std::printf("usage: capture_probe <out.skau> [seconds]\n"); return 2; }
    const int secs = argc > 2 ? std::atoi(argv[2]) : 20;
    std::map<DWORD, std::pair<DWORD, std::wstring>> procs;
    HANDLE s = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof pe;
    for (BOOL ok = ::Process32FirstW(s, &pe); ok; ok = ::Process32NextW(s, &pe)) procs[pe.th32ProcessID] = {pe.th32ParentProcessID, pe.szExeFile};
    ::CloseHandle(s);
    unsigned root = 0;
    for (const auto& [pid, p] : procs)
        if (_wcsicmp(p.second.c_str(), L"ms-teams.exe") == 0 &&
            !(procs.count(p.first) && _wcsicmp(procs[p.first].second.c_str(), L"ms-teams.exe") == 0))
            root = pid;
    if (!root) { std::printf("ms-teams.exe is not running\n"); return 1; }
    std::printf("capturing ms-teams.exe(%u) for %d s\n", root, secs);
    const long long start = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    auto w = wb::meeting::TrackWriter::create(argv[1], wb::meeting::TrackKind::remote, 16000, start);
    if (!w) { std::printf("writer: %s\n", w.error().message.c_str()); return 1; }
    std::shared_ptr<wb::meeting::TrackWriter> writer(std::move(*w));
    auto c = wb::meeting::AudioCapture::start({wb::meeting::CaptureSource::Kind::process, root}, writer);
    if (!c) { std::printf("capture: %s\n", c.error().message.c_str()); return 1; }
    std::this_thread::sleep_for(std::chrono::seconds(secs));
    (*c)->stop();
    std::printf("done: %llu samples, error '%s'\n", static_cast<unsigned long long>(writer->samples()), (*c)->error().c_str());
    return 0;
}
