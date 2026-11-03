// The string bundles built into the client (add-native-ui design D9) and the choice of language: an explicit
// setting wins, otherwise the Windows display language, otherwise English.
#pragma once

#include <string>
#include <string_view>

#include "ui/strings.h"

namespace wb::ui {

// The JSON of the built-in bundle for "cs", "de" or "en" (anything else gets English).
std::string_view bundle_json(std::string_view lang);

// "cs", "de" or "en" for a setting ("cs", "de", "en" or "system") and a system language tag such as "cs-CZ".
std::string resolve_language(std::string_view setting, std::string_view system_tag);

// The built-in bundle of a language; the build has checked it, so it always parses.
Strings load_bundle(std::string_view lang);

#ifdef _WIN32
// The Windows display language as a tag such as "cs-CZ" (Windows only).
std::string system_language();
#endif

}  // namespace wb::ui
