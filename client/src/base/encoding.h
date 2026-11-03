#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wb {

using bytes = std::vector<std::uint8_t>;

std::string base64_encode(const std::uint8_t* data, size_t n);
inline std::string base64_encode(const bytes& b) { return base64_encode(b.data(), b.size()); }
std::optional<bytes> base64_decode(std::string_view s);

// RFC 4648 section 5, without padding.
std::string base64url_encode(const std::uint8_t* data, size_t n);
inline std::string base64url_encode(const bytes& b) { return base64url_encode(b.data(), b.size()); }
inline std::string base64url_encode(std::string_view s) {
    return base64url_encode(reinterpret_cast<const std::uint8_t*>(s.data()), s.size());
}
std::optional<bytes> base64url_decode(std::string_view s);

std::string hex_encode(const std::uint8_t* data, size_t n);
inline std::string hex_encode(const bytes& b) { return hex_encode(b.data(), b.size()); }

inline std::string to_string(const bytes& b) { return std::string(b.begin(), b.end()); }

}  // namespace wb
