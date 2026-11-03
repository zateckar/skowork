// Thin Win32 RAII helpers (design D2a: in-house wrappers instead of WIL) and common queries.
#pragma once

#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>

namespace wb {

template <typename Traits>
class unique_res {
public:
    using value_type = typename Traits::type;

    unique_res() noexcept = default;
    explicit unique_res(value_type v) noexcept : v_(v) {}
    ~unique_res() { reset(); }
    unique_res(unique_res&& o) noexcept : v_(std::exchange(o.v_, Traits::invalid())) {}
    unique_res& operator=(unique_res&& o) noexcept {
        if (this != &o) reset(std::exchange(o.v_, Traits::invalid()));
        return *this;
    }
    unique_res(const unique_res&) = delete;
    unique_res& operator=(const unique_res&) = delete;

    [[nodiscard]] value_type get() const noexcept { return v_; }
    value_type* put() noexcept {
        reset();
        return &v_;
    }
    value_type release() noexcept { return std::exchange(v_, Traits::invalid()); }
    void reset(value_type v = Traits::invalid()) noexcept {
        if (v_ != Traits::invalid()) Traits::close(v_);
        v_ = v;
    }
    explicit operator bool() const noexcept { return v_ != Traits::invalid(); }

private:
    value_type v_ = Traits::invalid();
};

struct handle_traits {
    using type = HANDLE;
    static HANDLE invalid() noexcept { return nullptr; }
    static void close(HANDLE h) noexcept { ::CloseHandle(h); }
};
struct file_traits {
    using type = HANDLE;
    static HANDLE invalid() noexcept { return INVALID_HANDLE_VALUE; }
    static void close(HANDLE h) noexcept { ::CloseHandle(h); }
};
struct hlocal_traits {
    using type = HLOCAL;
    static HLOCAL invalid() noexcept { return nullptr; }
    static void close(HLOCAL h) noexcept { ::LocalFree(h); }
};
struct hkey_traits {
    using type = HKEY;
    static HKEY invalid() noexcept { return nullptr; }
    static void close(HKEY h) noexcept { ::RegCloseKey(h); }
};

using unique_handle = unique_res<handle_traits>;
using unique_file = unique_res<file_traits>;
using unique_hlocal = unique_res<hlocal_traits>;
using unique_hkey = unique_res<hkey_traits>;

std::wstring widen(std::string_view utf8);
std::string narrow(std::wstring_view wide);
std::string error_text(DWORD code);  // "0x... (message)"

std::wstring current_user_sid();  // "S-1-5-21-..."
std::wstring computer_name();
std::wstring user_name();         // SAM name of the current user
std::wstring user_principal();    // UPN if available (domain accounts), else DOMAIN\user
std::wstring local_appdata();     // %LOCALAPPDATA%
std::filesystem::path exe_path();
std::filesystem::path exe_dir();
std::string os_build();           // "10.0.22631.4317"
std::string hardware_class();     // "cpu:<n>c;ram:<n>GB"

// Creates `path` (and parents) with a protected DACL that grants full control only to the current user and
// SYSTEM, or re-applies that DACL to an existing directory (client/local-storage Owner-Only Access).
bool create_private_directory(const std::wstring& path, std::string& err);

// True if the DACL of `path` grants access only to the current user and SYSTEM and is protected.
bool is_private_directory(const std::wstring& path, std::string& detail);

}  // namespace wb
