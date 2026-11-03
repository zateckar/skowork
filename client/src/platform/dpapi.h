// DPAPI (CurrentUser) with additional entropy bound to the user SID (add-client-foundation design D6).
#pragma once

#include "base/encoding.h"
#include "base/result.h"

#include <string>
#include <string_view>

namespace wb {

Result<bytes> dpapi_protect(const bytes& plain, std::wstring_view purpose);
Result<bytes> dpapi_unprotect(const bytes& blob, std::wstring_view purpose);

}  // namespace wb
