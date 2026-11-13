#include "platform/pipe.h"

#include "platform/win.h"

#include <sddl.h>

#include <algorithm>

namespace wb {

std::wstring default_pipe_name() { return L"\\\\.\\pipe\\Skowork-" + current_user_sid(); }

bool pipe_write_frame(void* handle, const std::string& body) {
    const auto len = static_cast<DWORD>(body.size());
    DWORD w = 0;
    return ::WriteFile(handle, &len, 4, &w, nullptr) && w == 4 &&
           (len == 0 || (::WriteFile(handle, body.data(), len, &w, nullptr) && w == len));
}

bool pipe_read_frame(void* handle, std::string& body, size_t max_len) {
    auto read_exact = [&](void* p, DWORD n) {
        auto* b = static_cast<char*>(p);
        while (n) {
            DWORD got = 0;
            if (!::ReadFile(handle, b, n, &got, nullptr) || got == 0) return false;
            b += got;
            n -= got;
        }
        return true;
    };
    DWORD len = 0;
    if (!read_exact(&len, 4) || len > max_len) return false;
    body.assign(len, '\0');
    return len == 0 || read_exact(body.data(), len);
}

void* PipeServer::make_instance(bool first) {
    // Only the current user (no SYSTEM, no administrators) may connect.
    const std::wstring sddl = L"D:P(A;;GA;;;" + current_user_sid() + L")";
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!::ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &sd, nullptr))
        return INVALID_HANDLE_VALUE;
    unique_hlocal guard(sd);
    SECURITY_ATTRIBUTES sa{sizeof sa, sd, FALSE};
    return ::CreateNamedPipeW(name_.c_str(), PIPE_ACCESS_DUPLEX | (first ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0),
                              PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                              PIPE_UNLIMITED_INSTANCES, 65536, 65536, 0, &sa);
}

Result<std::unique_ptr<PipeServer>> PipeServer::create(const std::wstring& name) {
    std::unique_ptr<PipeServer> s(new PipeServer(name));
    s->first_ = s->make_instance(true);
    if (s->first_ == INVALID_HANDLE_VALUE) {
        const DWORD e = ::GetLastError();
        return make_error(e == ERROR_ACCESS_DENIED || e == ERROR_PIPE_BUSY ? "exists" : "pipe", error_text(e));
    }
    return s;
}

PipeServer::~PipeServer() { stop(); }

void PipeServer::start(Handler handler) {
    handler_ = std::move(handler);
    accept_thread_ = std::thread([this] { accept_loop(); });
}

void PipeServer::accept_loop() {
    void* pipe = first_;
    first_ = nullptr;
    while (!stopping_) {
        if (!pipe || pipe == INVALID_HANDLE_VALUE) pipe = make_instance(false);
        if (pipe == INVALID_HANDLE_VALUE) return;
        const BOOL connected = ::ConnectNamedPipe(pipe, nullptr) || ::GetLastError() == ERROR_PIPE_CONNECTED;
        if (stopping_ || !connected) {
            ::CloseHandle(pipe);
            return;
        }
        const std::lock_guard lock(workers_mx_);
        workers_.emplace_back([this, pipe] {
            std::string req;
            // A connection may carry several requests; it ends when the client closes it.
            while (!stopping_ && pipe_read_frame(pipe, req)) {
                if (!pipe_write_frame(pipe, handler_(req))) break;
            }
            ::FlushFileBuffers(pipe);
            ::DisconnectNamedPipe(pipe);
            ::CloseHandle(pipe);
        });
        pipe = nullptr;
    }
}

void PipeServer::stop() {
    if (stopping_.exchange(true)) return;
    if (accept_thread_.joinable()) {
        // MinGW std::thread has no Win32 handle for CancelSynchronousIo: connect once to unblock ConnectNamedPipe.
        unique_file h(::CreateFileW(name_.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr));
        h.reset();
        accept_thread_.join();
    }
    if (first_ && first_ != INVALID_HANDLE_VALUE) ::CloseHandle(first_);
    first_ = nullptr;
    std::vector<std::thread> workers;
    {
        const std::lock_guard lock(workers_mx_);
        workers.swap(workers_);
    }
    for (auto& t : workers)
        if (t.joinable()) t.join();
}

Result<std::string> pipe_call(const std::wstring& name, const std::string& request, unsigned timeout_ms) {
    // All instances may be busy for a moment; another client can also take the instance WaitNamedPipe reported.
    const ULONGLONG deadline = ::GetTickCount64() + timeout_ms;
    unique_file h;
    for (;;) {
        h.reset(::CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr));
        if (h) break;
        const DWORD err = ::GetLastError();
        const ULONGLONG now = ::GetTickCount64();
        if (err != ERROR_PIPE_BUSY || now >= deadline) return make_error("connect", error_text(err));
        ::WaitNamedPipeW(name.c_str(), static_cast<DWORD>(std::min<ULONGLONG>(deadline - now, 1000)));
    }
    if (!pipe_write_frame(h.get(), request)) return make_error("io", "write failed");
    std::string resp;
    if (!pipe_read_frame(h.get(), resp)) return make_error("io", "read failed");
    return resp;
}

}  // namespace wb
