#include "worker/handlers.h"

#include "extract/extract.h"
#include "knowledge/core.h"
#include "meeting/audio_file.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <vector>

namespace wb::worker {

Result<json> dispatch(const std::string& role, const std::string& method, const json& params) {
    if (method == "ping") return json{{"role", role}, {"pid", ::GetCurrentProcessId()}};
    if (role == "index" && method.rfind("index.", 0) == 0) return knowledge::index_request(method, params);
    if (role == "stt" && method.rfind("stt.", 0) == 0) return meeting::stt_request(method, params);
#ifdef WORKBENCH_DEV_ENDPOINTS
    // Test hooks (dev builds only): hold the worker busy, commit memory to exercise the budget, or crash.
    if (method == "crash") __builtin_trap();
    if (method == "sleep") {
        ::Sleep(static_cast<DWORD>(jint(params, "ms")));
        return json{{"slept", jint(params, "ms")}};
    }
    if (method == "alloc") {
        const auto mb = static_cast<size_t>(jint(params, "mb"));
        std::vector<char*> blocks;
        for (size_t i = 0; i < mb; ++i) {
            auto* p = static_cast<char*>(::VirtualAlloc(nullptr, 1 << 20, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
            if (!p) ::ExitProcess(0xE0000001);
            std::memset(p, 1, 1 << 20);
            blocks.push_back(p);
        }
        for (char* p : blocks) ::VirtualFree(p, 0, MEM_RELEASE);
        return json{{"allocated_mb", mb}};
    }
#else
    (void)params;
#endif
    return make_error("unknown_method", "the " + role + " worker has no method " + method);
}

namespace {

void write_all(HANDLE h, std::string_view s) {
    size_t done = 0;
    while (done < s.size()) {
        DWORD n = 0;
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(s.size() - done, 1u << 20));
        if (!::WriteFile(h, s.data() + done, chunk, &n, nullptr) || n == 0) return;
        done += n;
    }
}

}  // namespace

int extract_once(const std::string& kind, void* in, void* out, void* err) {
    std::string doc;
    char buf[64 * 1024];
    for (;;) {
        DWORD n = 0;
        if (!::ReadFile(static_cast<HANDLE>(in), buf, sizeof buf, &n, nullptr) || n == 0) break;
        if (doc.size() + n > (64u << 20)) {
            write_all(static_cast<HANDLE>(err), "too_large: the document is larger than 64 MB");
            return 1;
        }
        doc.append(buf, n);
    }
#ifdef WORKBENCH_DEV_ENDPOINTS
    // Test hooks (dev builds only): a parser that crashes, hangs or runs away with memory.
    if (kind == "test-crash") __builtin_trap();
    if (kind == "test-hang") ::Sleep(INFINITE);
    if (kind == "test-alloc") {
        for (;;) {
            auto* p = static_cast<char*>(::VirtualAlloc(nullptr, 64 << 20, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
            if (!p) ::ExitProcess(0xE0000001);
            std::memset(p, 1, 64 << 20);
        }
    }
#endif
    auto text = extract::extract_document(kind, doc);
    if (!text) {
        write_all(static_cast<HANDLE>(err), text.error().code + ": " + text.error().message);
        return 1;
    }
    write_all(static_cast<HANDLE>(out), *text);
    return 0;
}

}  // namespace wb::worker
