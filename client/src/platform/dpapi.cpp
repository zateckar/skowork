#include "platform/dpapi.h"

#include "platform/win.h"

#include <dpapi.h>

namespace wb {
namespace {

std::wstring entropy_for(std::wstring_view purpose) {
    return L"Workbench/" + std::wstring(purpose) + L"/" + current_user_sid();
}

DATA_BLOB blob_of(const void* p, size_t n) {
    return DATA_BLOB{static_cast<DWORD>(n), static_cast<BYTE*>(const_cast<void*>(p))};
}

}  // namespace

Result<bytes> dpapi_protect(const bytes& plain, std::wstring_view purpose) {
    const std::wstring ent = entropy_for(purpose);
    DATA_BLOB in = blob_of(plain.data(), plain.size());
    DATA_BLOB entropy = blob_of(ent.data(), ent.size() * sizeof(wchar_t));
    DATA_BLOB out{};
    if (!::CryptProtectData(&in, L"Workbench", &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out))
        return make_error("dpapi", "CryptProtectData: " + error_text(::GetLastError()));
    bytes blob(out.pbData, out.pbData + out.cbData);
    ::LocalFree(out.pbData);
    return blob;
}

Result<bytes> dpapi_unprotect(const bytes& blob, std::wstring_view purpose) {
    const std::wstring ent = entropy_for(purpose);
    DATA_BLOB in = blob_of(blob.data(), blob.size());
    DATA_BLOB entropy = blob_of(ent.data(), ent.size() * sizeof(wchar_t));
    DATA_BLOB out{};
    if (!::CryptUnprotectData(&in, nullptr, &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out))
        return make_error("dpapi", "CryptUnprotectData: " + error_text(::GetLastError()));
    bytes plain(out.pbData, out.pbData + out.cbData);
    ::SecureZeroMemory(out.pbData, out.cbData);
    ::LocalFree(out.pbData);
    return plain;
}

}  // namespace wb
