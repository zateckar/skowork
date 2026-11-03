#include "tools/quarantine.h"

#include "base/strings.h"
#include "base/url.h"
#include "platform/win.h"

#include <array>
#include <chrono>
#include <ctime>
#include <filesystem>

namespace wb::tools {

namespace {

bool write_whole(const std::wstring& path, std::string_view data, DWORD disposition) {
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, disposition, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool ok = true;
    size_t done = 0;
    while (ok && done < data.size()) {
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(data.size() - done, 1u << 24));
        DWORD wrote = 0;
        ok = ::WriteFile(h, data.data() + done, chunk, &wrote, nullptr) && wrote == chunk;
        done += wrote;
    }
    ::CloseHandle(h);
    return ok;
}

std::string timestamp() {
    const std::time_t t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{};
    gmtime_s(&tm, &t);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y%m%d-%H%M%S", &tm);
    return buf;
}

std::string percent_decode(std::string_view s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() && std::isxdigit(static_cast<unsigned char>(s[i + 1])) &&
            std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
            out += static_cast<char>(std::stoi(std::string(s.substr(i + 1, 2)), nullptr, 16));
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}

}  // namespace

std::string safe_file_name(std::string_view name) {
    std::string out;
    for (const char c : name) {
        const auto u = static_cast<unsigned char>(c);
        if (u < 0x20 || std::string_view("<>:\"/\\|?*").find(c) != std::string_view::npos) out += '_';
        else out += c;
    }
    while (!out.empty() && (out.back() == '.' || out.back() == ' ')) out.pop_back();
    while (!out.empty() && (out.front() == '.' || out.front() == ' ')) out.erase(0, 1);
    if (out.size() > 120) {
        // Keep the extension, cut the stem at a UTF-8 boundary.
        const auto dot = out.rfind('.');
        const std::string ext = dot != std::string::npos && out.size() - dot <= 16 ? out.substr(dot) : std::string{};
        size_t keep = 120 - ext.size();
        while (keep > 0 && (static_cast<unsigned char>(out[keep]) & 0xC0) == 0x80) --keep;
        out = out.substr(0, keep) + ext;
    }
    const std::string stem = to_lower(out.substr(0, out.find('.')));
    static const std::array<const char*, 22> kDevices = {"con",  "prn",  "aux",  "nul",  "com1", "com2", "com3", "com4",
                                                         "com5", "com6", "com7", "com8", "com9", "lpt1", "lpt2", "lpt3",
                                                         "lpt4", "lpt5", "lpt6", "lpt7", "lpt8", "lpt9"};
    for (const char* d : kDevices)
        if (stem == d) out = "_" + out;
    return out.empty() ? "download" : out;
}

std::string download_name(std::string_view content_disposition, std::string_view url) {
    // filename*=UTF-8''name wins over filename="name".
    const std::string cd(content_disposition);
    const std::string lower = to_lower(cd);
    auto value_after = [&](const std::string& key) -> std::string {
        const auto at = lower.find(key);
        if (at == std::string::npos) return {};
        size_t i = at + key.size();
        std::string v;
        if (i < cd.size() && cd[i] == '"') {
            const auto end = cd.find('"', i + 1);
            v = cd.substr(i + 1, end == std::string::npos ? std::string::npos : end - i - 1);
        } else {
            const auto end = cd.find(';', i);
            v = cd.substr(i, end == std::string::npos ? std::string::npos : end - i);
        }
        return std::string(trim(v));
    };
    if (std::string v = value_after("filename*="); !v.empty()) {
        if (const auto q = v.find("''"); q != std::string::npos) v = v.substr(q + 2);
        return percent_decode(v);
    }
    if (std::string v = value_after("filename="); !v.empty()) return v;
    if (const auto u = parse_url(url)) {
        std::string path = u->path;
        if (const auto q = path.find_first_of("?#"); q != std::string::npos) path.resize(q);
        const auto slash = path.rfind('/');
        const std::string last = percent_decode(slash == std::string::npos ? path : path.substr(slash + 1));
        if (!last.empty()) return last;
    }
    return "download";
}

Result<QuarantinedFile> quarantine_save(const std::string& dir, std::string_view suggested_name, std::string_view bytes,
                                        std::string_view source_url, std::string_view referrer_url) {
    std::error_code ec;
    const std::filesystem::path folder(widen(dir));
    std::filesystem::create_directories(folder, ec);
    if (ec) return make_error("io", "cannot create the quarantine folder " + dir + ": " + ec.message());
    const std::string base = timestamp() + "-" + safe_file_name(suggested_name);
    std::wstring path;
    for (int n = 0;; ++n) {
        std::string name = base;
        if (n > 0) {
            const auto dot = name.rfind('.');
            const std::string suffix = " (" + std::to_string(n) + ")";
            name = dot == std::string::npos || dot == 0 ? name + suffix : name.substr(0, dot) + suffix + name.substr(dot);
        }
        path = (folder / widen(name)).wstring();
        HANDLE h = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            ::CloseHandle(h);
            break;
        }
        if (::GetLastError() != ERROR_FILE_EXISTS || n >= 100) return make_error("io", "cannot create a file in the quarantine folder: " + error_text(::GetLastError()));
    }
    // Mark of the web first, so the file never exists with content and without the mark.
    std::string zone = "[ZoneTransfer]\r\nZoneId=3\r\n";
    if (!referrer_url.empty()) zone += "ReferrerUrl=" + std::string(referrer_url) + "\r\n";
    if (!source_url.empty()) zone += "HostUrl=" + std::string(source_url) + "\r\n";
    if (!write_whole(path + L":Zone.Identifier", zone, CREATE_ALWAYS) || !write_whole(path, bytes, TRUNCATE_EXISTING)) {
        const DWORD err = ::GetLastError();
        ::DeleteFileW(path.c_str());
        return make_error("io", "cannot write the downloaded file: " + error_text(err));
    }
    return QuarantinedFile{narrow(path), bytes.size()};
}

}  // namespace wb::tools
