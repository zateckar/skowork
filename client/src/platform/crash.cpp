#include "platform/crash.h"

#include <windows.h>
#include <dbghelp.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <exception>
#include <random>

namespace wb::crash {
namespace {

constexpr DWORD kTerminateException = 0xE0000002;  // std::terminate, raised so the filter sees it

// Everything the writer needs is prepared at install time: at crash time the heap may be corrupt.
struct State {
    wchar_t prefix[MAX_PATH * 2] = {};  // dir\process- (core)
    HANDLE file = nullptr;              // the inherited dump file (worker)
    HANDLE request = nullptr, done = nullptr;
    EXCEPTION_POINTERS* ep = nullptr;
    DWORD tid = 0;
    std::atomic<bool> enabled{true};
    std::atomic<bool> claimed{false};
};
State g_state;
std::atomic<bool> g_installed{false};

DWORD WINAPI writer(void*) {
    ::WaitForSingleObject(g_state.request, INFINITE);
    HANDLE f = g_state.file;
    bool own = false;
    if (!f) {
        wchar_t path[MAX_PATH * 2 + 32];
        FILETIME ft;
        ::GetSystemTimeAsFileTime(&ft);
        const unsigned long long ms = ((static_cast<unsigned long long>(ft.dwHighDateTime) << 32 | ft.dwLowDateTime) - 116444736000000000ULL) / 10000;
        std::swprintf(path, std::size(path), L"%ls%llu.dmp", g_state.prefix, ms);
        f = ::CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        own = true;
    }
    if (f && f != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION mei{g_state.tid, g_state.ep, FALSE};
        ::MiniDumpWriteDump(::GetCurrentProcess(), ::GetCurrentProcessId(), f, static_cast<MINIDUMP_TYPE>(dump_type()), &mei, nullptr, nullptr);
        ::FlushFileBuffers(f);
        if (own) ::CloseHandle(f);
    }
    ::SetEvent(g_state.done);
    return 0;
}

LONG WINAPI filter(EXCEPTION_POINTERS* ep) {
    if (!g_state.enabled) return EXCEPTION_CONTINUE_SEARCH;
    if (g_state.claimed.exchange(true)) {  // another thread is already being dumped
        ::WaitForSingleObject(g_state.done, 60000);
        return EXCEPTION_EXECUTE_HANDLER;
    }
    g_state.ep = ep;
    g_state.tid = ::GetCurrentThreadId();
    ::SetEvent(g_state.request);
    ::WaitForSingleObject(g_state.done, 60000);
    return EXCEPTION_EXECUTE_HANDLER;
}

void start() {
    if (g_installed.exchange(true)) return;
    g_state.request = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_state.done = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    // The writer has its own stack, so a stack overflow can still be dumped.
    if (HANDLE t = ::CreateThread(nullptr, 256 * 1024, writer, nullptr, 0, nullptr)) ::CloseHandle(t);
    ::SetUnhandledExceptionFilter(filter);
    std::set_terminate([] {
        ::RaiseException(kTerminateException, EXCEPTION_NONCONTINUABLE, 0, nullptr);
        std::abort();
    });
}

std::string hex_code(DWORD code) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "0x%08lX", static_cast<unsigned long>(code));
    return buf;
}

}  // namespace

unsigned long dump_type() { return MiniDumpFilterMemory | MiniDumpWithoutOptionalData | MiniDumpFilterModulePaths; }

void install(const std::filesystem::path& dir, std::string_view process) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const std::wstring prefix = (dir / widen(process)).wstring() + L"-";
    if (prefix.size() >= std::size(g_state.prefix)) return;
    std::wmemcpy(g_state.prefix, prefix.c_str(), prefix.size() + 1);
    start();
}

void install_to_handle(HANDLE file) {
    if (!file || file == INVALID_HANDLE_VALUE) return;
    g_state.file = file;
    start();
}

void set_enabled(bool on) { g_state.enabled = on; }

Result<DumpSlot> open_dump_slot(const std::filesystem::path& dir, std::string_view process) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::random_device rd;
    char tail[16];
    std::snprintf(tail, sizeof tail, "%08x", rd());
    DumpSlot s;
    s.path = dir / (widen(process) + L"-" + std::to_wstring(::GetTickCount64()) + L"-" + widen(tail) + L".dmp");
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    s.file.reset(::CreateFileW(s.path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, &sa, CREATE_NEW,
                               FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!s.file) return make_error("crash", "cannot create the dump file: " + error_text(::GetLastError()));
    return s;
}

std::wstring dump_argument(const DumpSlot& slot) {
    return L"--crash-dump " + std::to_wstring(reinterpret_cast<std::uintptr_t>(slot.file.get()));
}

HANDLE parse_dump_argument(std::wstring_view value) {
    std::uintptr_t v = 0;
    if (value.empty() || value.size() > 20) return nullptr;
    for (const wchar_t c : value) {
        if (c < L'0' || c > L'9') return nullptr;
        v = v * 10 + static_cast<std::uintptr_t>(c - L'0');
    }
    return reinterpret_cast<HANDLE>(v);
}

std::optional<std::filesystem::path> take_dump(DumpSlot& slot) {
    slot.file.reset();
    std::error_code ec;
    const auto size = std::filesystem::file_size(slot.path, ec);
    if (ec) return std::nullopt;
    if (size == 0) {
        std::filesystem::remove(slot.path, ec);
        return std::nullopt;
    }
    return slot.path;
}

Result<Summary> summarize(const std::filesystem::path& dump) {
    unique_file f(::CreateFileW(dump.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr));
    if (!f) return make_error("crash", "cannot open the dump: " + error_text(::GetLastError()));
    LARGE_INTEGER li{};
    if (!::GetFileSizeEx(f.get(), &li) || li.QuadPart == 0) return make_error("crash", "the dump is empty");
    unique_handle map(::CreateFileMappingW(f.get(), nullptr, PAGE_READONLY, 0, 0, nullptr));
    if (!map) return make_error("crash", "cannot map the dump");
    auto* base = static_cast<const unsigned char*>(::MapViewOfFile(map.get(), FILE_MAP_READ, 0, 0, 0));
    if (!base) return make_error("crash", "cannot map the dump");
    const auto size = static_cast<size_t>(li.QuadPart);
    Summary s;
    std::vector<std::pair<std::string, std::pair<ULONG64, ULONG64>>> ranges;
    void* stream = nullptr;
    ULONG stream_size = 0;
    // Stream records are only 4-byte aligned: every field is copied out rather than read through a struct pointer.
    auto read = [&](size_t off, void* out, size_t n) {
        if (off > size || n > size - off) return false;
        std::memcpy(out, base + off, n);
        return true;
    };
    auto offset_of = [&](const void* p) { return static_cast<size_t>(static_cast<const unsigned char*>(p) - base); };
    if (::MiniDumpReadDumpStream(const_cast<unsigned char*>(base), ModuleListStream, nullptr, &stream, &stream_size) && stream) {
        const size_t list = offset_of(stream);
        ULONG32 count = 0;
        read(list, &count, sizeof count);
        for (ULONG32 i = 0; i < count && i < 4096; ++i) {
            MINIDUMP_MODULE m;
            if (!read(list + sizeof(ULONG32) + i * sizeof(MINIDUMP_MODULE), &m, sizeof m)) break;
            ULONG32 bytes = 0;
            std::string file;
            if (read(m.ModuleNameRva, &bytes, sizeof bytes) && bytes / 2 < 1024) {
                std::wstring w(bytes / 2, L'\0');
                if (read(static_cast<size_t>(m.ModuleNameRva) + sizeof(ULONG32), w.data(), bytes)) {
                    std::wstring_view v(w);
                    if (const size_t slash = v.find_last_of(L"\\/"); slash != std::wstring_view::npos) v.remove_prefix(slash + 1);
                    file = narrow(v);
                }
            }
            s.modules.push_back(file);
            ranges.push_back({file, {m.BaseOfImage, m.BaseOfImage + m.SizeOfImage}});
        }
    }
    if (::MiniDumpReadDumpStream(const_cast<unsigned char*>(base), ThreadListStream, nullptr, &stream, &stream_size) && stream) {
        ULONG32 threads = 0;
        read(offset_of(stream), &threads, sizeof threads);
        s.threads = static_cast<int>(threads);
    }
    if (::MiniDumpReadDumpStream(const_cast<unsigned char*>(base), ExceptionStream, nullptr, &stream, &stream_size) && stream) {
        MINIDUMP_EXCEPTION_STREAM ex;
        if (read(offset_of(stream), &ex, sizeof ex)) {
            s.exception_code = hex_code(ex.ExceptionRecord.ExceptionCode);
            const ULONG64 addr = ex.ExceptionRecord.ExceptionAddress;
            for (const auto& [name, r] : ranges)
                if (addr >= r.first && addr < r.second) s.module = name;
        }
    }    ::UnmapViewOfFile(base);
    return s;
}

}  // namespace wb::crash
