#include "meeting/wav.h"

#include <cstring>
#include <filesystem>
#include <fstream>

namespace wb::meeting {
namespace {

void put32(std::vector<char>& b, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}
void put16(std::vector<char>& b, std::uint16_t v) {
    b.push_back(static_cast<char>(v & 0xFF));
    b.push_back(static_cast<char>((v >> 8) & 0xFF));
}
std::uint32_t get32(const unsigned char* p) {
    return static_cast<std::uint32_t>(p[0]) | static_cast<std::uint32_t>(p[1]) << 8 | static_cast<std::uint32_t>(p[2]) << 16 |
           static_cast<std::uint32_t>(p[3]) << 24;
}
std::uint16_t get16(const unsigned char* p) { return static_cast<std::uint16_t>(p[0] | p[1] << 8); }

}  // namespace

Status write_wav(const std::filesystem::path& file, const std::vector<std::int16_t>& pcm, int sample_rate) {
    if (sample_rate <= 0) return make_error("invalid", "The sample rate must be positive.");
    const auto data_bytes = static_cast<std::uint32_t>(pcm.size() * sizeof(std::int16_t));
    std::vector<char> h;
    h.insert(h.end(), {'R', 'I', 'F', 'F'});
    put32(h, 36 + data_bytes);
    h.insert(h.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
    put32(h, 16);
    put16(h, 1);  // PCM
    put16(h, 1);  // mono
    put32(h, static_cast<std::uint32_t>(sample_rate));
    put32(h, static_cast<std::uint32_t>(sample_rate) * 2);
    put16(h, 2);
    put16(h, 16);
    h.insert(h.end(), {'d', 'a', 't', 'a'});
    put32(h, data_bytes);
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out) return make_error("write_failed", "Cannot write " + file.filename().string());
    out.write(h.data(), static_cast<std::streamsize>(h.size()));
    for (const std::int16_t s : pcm) {  // little-endian regardless of the host
        const auto u = static_cast<std::uint16_t>(s);
        const char b[2] = {static_cast<char>(u & 0xFF), static_cast<char>(u >> 8)};
        out.write(b, 2);
    }
    if (!out) return make_error("write_failed", "Cannot write " + file.filename().string() + " (is the disk full?)");
    return {};
}

Result<std::vector<std::int16_t>> read_wav(const std::filesystem::path& file, int* sample_rate) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return make_error("io", "Cannot read " + file.filename().string());
    unsigned char h[44];
    in.read(reinterpret_cast<char*>(h), sizeof h);
    if (in.gcount() != 44 || std::memcmp(h, "RIFF", 4) != 0 || std::memcmp(h + 8, "WAVE", 4) != 0 || get16(h + 20) != 1 ||
        get16(h + 22) != 1 || get16(h + 34) != 16 || std::memcmp(h + 36, "data", 4) != 0)
        return make_error("corrupt", "Not a 16-bit mono PCM WAV file.");
    if (sample_rate) *sample_rate = static_cast<int>(get32(h + 24));
    const std::uint32_t n = get32(h + 40) / 2;
    // The header is checked against the file before anything is allocated (review minor 9).
    std::error_code ec;
    const auto size = std::filesystem::file_size(file, ec);
    if (ec || size < 44 || static_cast<std::uint64_t>(n) * 2 > size - 44) return make_error("corrupt", "The WAV file is shorter than its header says.");
    std::vector<std::int16_t> pcm(n);
    for (std::uint32_t i = 0; i < n; ++i) {
        unsigned char b[2];
        in.read(reinterpret_cast<char*>(b), 2);
        if (in.gcount() != 2) return make_error("corrupt", "The WAV file is shorter than its header says.");
        pcm[i] = static_cast<std::int16_t>(get16(b));
    }
    return pcm;
}

}  // namespace wb::meeting
