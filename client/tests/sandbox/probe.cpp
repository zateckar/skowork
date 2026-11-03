// A helper the sandbox tests run inside the sandbox: each mode exercises one property (limits, token, environment,
// network, output, containment) and prints what it observed.
#include "platform/win.h"
#include "sandbox/sandbox.h"

#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#include <fcntl.h>
#include <io.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

using namespace wb;

namespace {

std::wstring self() { return exe_path().wstring(); }

// Starts `probe <args>` as a plain child and returns its process handle.
HANDLE spawn(const std::wstring& args, DWORD flags = 0) {
    std::wstring cmd = L"\"" + self() + L"\" " + args;
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    if (!::CreateProcessW(self().c_str(), cmd.data(), nullptr, nullptr, FALSE, flags | CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return nullptr;
    ::CloseHandle(pi.hThread);
    return pi.hProcess;
}

int token_report() {
    HANDLE t = nullptr;
    ::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &t);
    BYTE admins[SECURITY_MAX_SID_SIZE];
    DWORD size = sizeof admins;
    ::CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, admins, &size);
    DWORD n = 0;
    ::GetTokenInformation(t, TokenGroups, nullptr, 0, &n);
    std::vector<BYTE> buf(n);
    ::GetTokenInformation(t, TokenGroups, buf.data(), n, &n);
    const auto* g = reinterpret_cast<TOKEN_GROUPS*>(buf.data());
    const char* state = "absent";
    for (DWORD i = 0; i < g->GroupCount; ++i)
        if (::EqualSid(g->Groups[i].Sid, admins))
            state = (g->Groups[i].Attributes & SE_GROUP_USE_FOR_DENY_ONLY) ? "deny-only" : "enabled";
    ::GetTokenInformation(t, TokenPrivileges, nullptr, 0, &n);
    std::vector<BYTE> pb(n);
    ::GetTokenInformation(t, TokenPrivileges, pb.data(), n, &n);
    std::printf("admins=%s privileges=%lu\n", state, reinterpret_cast<TOKEN_PRIVILEGES*>(pb.data())->PrivilegeCount);
    return 0;
}

int connect_report(const char* host, const char* port) {
    WSADATA w;
    ::WSAStartup(MAKEWORD(2, 2), &w);
    addrinfo hints{};
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (::getaddrinfo(host, port, &hints, &res) != 0) {
        std::printf("failed resolve %d\n", ::WSAGetLastError());
        return 0;
    }
    SOCKET s = ::socket(res->ai_family, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        std::printf("failed socket %d\n", ::WSAGetLastError());
        return 0;
    }
    DWORD to = 5000;
    ::setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&to), sizeof to);
    if (::connect(s, res->ai_addr, static_cast<int>(res->ai_addrlen)) == 0) std::printf("connected\n");
    else std::printf("failed connect %d\n", ::WSAGetLastError());
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    const std::string mode = argv[1];
    const auto arg = [&](int i) { return argc > i ? std::string(argv[i]) : std::string(); };
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
    _setmode(_fileno(stdin), _O_BINARY);

    if (mode == "exit") return std::atoi(arg(2).c_str());
    if (mode == "echo") {
        char buf[4096];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, stdin)) > 0) std::fwrite(buf, 1, n, stdout);
        return 0;
    }
    if (mode == "sleep") {
        ::Sleep(static_cast<DWORD>(std::atoi(arg(2).c_str())));
        return 0;
    }
    if (mode == "alloc") {  // touch N MB in 1 MB blocks; report a failed allocation, then wait to be stopped
        const int mb = std::atoi(arg(2).c_str());
        std::vector<char*> blocks;
        for (int i = 0; i < mb; ++i) {
            char* p = static_cast<char*>(::VirtualAlloc(nullptr, 1 << 20, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
            if (!p) {
                std::printf("alloc-failed at %d MB\n", i);
                ::Sleep(30000);
                return 3;
            }
            std::memset(p, 1, 1 << 20);
            blocks.push_back(p);
        }
        std::printf("allocated %d MB\n", mb);
        return 0;
    }
    if (mode == "spawn") {  // start N sleeping children
        const int n = std::atoi(arg(2).c_str());
        int ok = 0;
        for (int i = 0; i < n; ++i)
            if (spawn(L"sleep 60000")) ++ok;
        std::printf("spawned %d\n", ok);
        ::Sleep(30000);
        return 0;
    }
    if (mode == "tree") {  // a grandchild that would outlive us, its pid written to a file
        HANDLE h = spawn(L"sleep 600000");
        std::ofstream(arg(2)) << ::GetProcessId(h);
        ::Sleep(600000);
        return 0;
    }
    if (mode == "host") {  // runs `tree` in the sandbox, then dies abruptly once the grandchild exists
        const std::string pidfile = arg(2);
        std::thread([pidfile] {
            sandbox::Sandbox sb;
            sandbox::ProcessSpec spec;
            spec.executable = self();
            spec.arguments = L"tree \"" + widen(pidfile) + L"\"";
            spec.network = sandbox::Network::direct;
            (void)sb.run(spec);
        }).detach();
        for (int i = 0; i < 200; ++i) {
            if (std::ifstream(pidfile).peek() != EOF) ::TerminateProcess(::GetCurrentProcess(), 9);
            ::Sleep(50);
        }
        return 4;
    }
    if (mode == "breakaway") {
        HANDLE h = spawn(L"sleep 10000", CREATE_BREAKAWAY_FROM_JOB);
        if (!h) {
            std::printf("breakaway-refused %lu\n", ::GetLastError());
            return 0;
        }
        BOOL in_job = FALSE;
        ::IsProcessInJob(h, nullptr, &in_job);
        std::printf(in_job ? "child-in-job\n" : "child-outside-job\n");
        ::TerminateProcess(h, 0);
        return 0;
    }
    if (mode == "env") {
        wchar_t* block = ::GetEnvironmentStringsW();
        for (wchar_t* p = block; *p; p += std::wcslen(p) + 1) std::printf("%s\n", narrow(p).c_str());
        ::FreeEnvironmentStringsW(block);
        return 0;
    }
    if (mode == "token") return token_report();
    if (mode == "connect") return connect_report(arg(2).c_str(), arg(3).c_str());
    if (mode == "print") {  // N MB to stdout, a little to stderr
        const std::string chunk(1 << 20, 'x');
        const int mb = std::atoi(arg(2).c_str());
        for (int i = 0; i < mb; ++i) std::fwrite(chunk.data(), 1, chunk.size(), stdout);
        std::fputs("done\n", stderr);
        return 0;
    }
    if (mode == "oem") {  // "Grüße" in code page 850/437 bytes
        std::fwrite("Gr\x81\xE1" "e\n", 1, 6, stdout);
        return 0;
    }
    if (mode == "write") {  // create a file, report the outcome
        std::FILE* f = std::fopen(arg(2).c_str(), "wb");
        std::printf(f ? "written\n" : "write-failed\n");
        if (f) std::fclose(f);
        return 0;
    }
    return 2;
}
