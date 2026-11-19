#include "meeting/speech_package.h"

#include "base/encoding.h"
#include "base/json.h"
#include "base/strings.h"
#include "platform/crypto.h"
#include "platform/http.h"
#include "platform/win.h"

#include <miniz.h>

#include <bcrypt.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <set>

namespace wb::meeting {
namespace {

namespace fs = std::filesystem;

constexpr size_t kChunk = 1u << 20;

bool is_hex64(std::string_view s) {
    return s.size() == 64 && std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isdigit(c) || (c >= 'a' && c <= 'f'); });
}

bool is_https(std::string_view url) { return url.size() > 8 && url.substr(0, 8) == "https://"; }

// The lower-case host of an https URL without credentials or port ("" when it has none).
std::string url_host(std::string_view url) {
    const size_t p = url.find("://");
    if (p == std::string_view::npos) return {};
    std::string_view rest = url.substr(p + 3);
    std::string_view authority = rest.substr(0, rest.find_first_of("/?#"));
    if (const size_t at = authority.rfind('@'); at != std::string_view::npos) authority = authority.substr(at + 1);
    if (const size_t colon = authority.rfind(':'); colon != std::string_view::npos) authority = authority.substr(0, colon);
    return to_lower(authority);
}

// User information in a URL would be sent as credentials; a package URL never carries any.
bool has_userinfo(std::string_view url) {
    const size_t p = url.find("://");
    if (p == std::string_view::npos) return false;
    const std::string_view rest = url.substr(p + 3);
    return rest.substr(0, rest.find_first_of("/?#")).find('@') != std::string_view::npos;
}

// SHA-256 over chunks, through CNG (the whole archive is never held in memory).
class Sha256 {
public:
    Sha256() {
        if (BCryptOpenAlgorithmProvider(&alg_, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return;
        DWORD objlen = 0, n = 0;
        if (BCryptGetProperty(alg_, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objlen), sizeof objlen, &n, 0) != 0) return;
        obj_.resize(objlen);
        if (BCryptCreateHash(alg_, &hash_, obj_.data(), objlen, nullptr, 0, 0) != 0) return;
        ok_ = true;
    }
    ~Sha256() {
        if (hash_) BCryptDestroyHash(hash_);
        if (alg_) BCryptCloseAlgorithmProvider(alg_, 0);
    }
    Sha256(const Sha256&) = delete;
    Sha256& operator=(const Sha256&) = delete;
    [[nodiscard]] bool ok() const { return ok_; }
    bool update(const char* p, size_t n) {
        return BCryptHashData(hash_, reinterpret_cast<PUCHAR>(const_cast<char*>(p)), static_cast<ULONG>(n), 0) == 0;
    }
    std::string finish() {
        std::uint8_t d[32] = {};
        BCryptFinishHash(hash_, d, sizeof d, 0);
        return hex_encode(d, sizeof d);
    }

private:
    BCRYPT_ALG_HANDLE alg_ = nullptr;
    BCRYPT_HASH_HANDLE hash_ = nullptr;
    std::vector<UCHAR> obj_;
    bool ok_ = false;
};

// Removes a folder when the scope ends unless it was moved into place.
struct Scope {
    fs::path path;
    bool keep = false;
    ~Scope() {
        if (keep) return;
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

// The archive file as random-access bytes for SpeechArchive (the archive is never loaded whole).
SpeechArchiveRead file_reader(const std::shared_ptr<std::ifstream>& in) {
    return [in](std::uint64_t ofs, void* buf, size_t n) -> size_t {
        in->clear();
        in->seekg(static_cast<std::streamoff>(ofs));
        if (!*in) return 0;
        in->read(static_cast<char*>(buf), static_cast<std::streamsize>(n));
        return static_cast<size_t>(in->gcount());
    };
}

std::uint64_t free_bytes(const SpeechLimits& lim, const fs::path& at) {
    if (lim.free_space) return lim.free_space(at);
    ULARGE_INTEGER avail{};
    if (!::GetDiskFreeSpaceExW(at.c_str(), &avail, nullptr, nullptr)) return ~0ull;  // unknown: writes report failures
    return avail.QuadPart;
}

// Fails with disk_full unless `need` bytes fit and kSpeechDiskReserve stays free.
Status require_space(const SpeechLimits& lim, const fs::path& at, std::uint64_t need) {
    const std::uint64_t avail = free_bytes(lim, at);
    if (avail >= need && avail - need >= kSpeechDiskReserve) return {};
    const std::uint64_t missing = need + kSpeechDiskReserve - std::min(avail, need + kSpeechDiskReserve);
    return make_error("disk_full", "Not enough disk space for the speech package: free " + std::to_string((missing + (1u << 20) - 1) >> 20) +
                                       " MB more and try again.");
}
bool package_complete(const fs::path& dir, const std::string& name) {
    std::error_code ec;
    std::ifstream in(dir / L"complete.json", std::ios::binary);
    if (!in) return false;
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const auto j = parse_json(text);
    if (!j || jstr(*j, "version") != name || jint(*j, "schema") != kSpeechSchema || jstr(*j, "package") != kSpeechPackageName ||
        jstr(*j, "architecture") != kSpeechArchitecture)
        return false;
    for (const char* req : kSpeechRequiredFiles)
        if (!fs::is_regular_file(dir / L"models" / L"stt" / widen(req), ec)) return false;
    return true;
}

Status ensure_store(const fs::path& root) {
    std::string err;
    if (!create_private_directory(speech_root(root).wstring(), err)) return make_error("io", "the speech folder cannot be created: " + err);
    return {};
}

// A GET without credentials. Redirects are followed by hand: each target must be https on an approved host.
Status fetch(const std::string& start, const std::set<std::string>& hosts, const std::shared_ptr<CancelToken>& cancel,
             const std::function<bool(std::string_view)>& on_data, const SpeechTransport& send) {
    std::string url = start;
    for (int hop = 0; hop < 5; ++hop) {
        if (!is_https(url) || has_userinfo(url) || !hosts.count(url_host(url)))
            return make_error("speech_redirect", "the speech package moved to an origin that is not approved");
        HttpRequest req;
        req.url = url;
        req.follow_redirects = false;
        req.cancel = cancel;
        req.connect_timeout = std::chrono::milliseconds(15000);
        req.idle_timeout = std::chrono::milliseconds(60000);
        req.stream_success_only = true;
        req.max_body = 1u << 20;
        req.on_chunk = on_data;
        auto res = send ? send(req) : http().send(req);
        if (!res) return res.error();
        const int s = res->status;
        if (s == 301 || s == 302 || s == 303 || s == 307 || s == 308) {
            const std::string loc = res->header("location");
            if (loc.empty()) return make_error("http", "the speech package redirect has no location");
            if (is_https(loc)) url = loc;
            else if (!loc.empty() && loc[0] == '/') url = url.substr(0, url.find('/', 8)) + loc;
            else return make_error("speech_redirect", "the speech package redirect is not an https URL");
            continue;
        }
        if (s < 200 || s > 299) return make_error("http", "the speech package server answered " + std::to_string(s));
        return {};
    }
    return make_error("speech_redirect", "the speech package has too many redirects");
}

}  // namespace


fs::path speech_root(const fs::path& data_root) { return data_root / L"speech"; }

Result<fs::path> active_speech_dir(const fs::path& data_root, std::string* version) {
    const fs::path pkgs = speech_root(data_root) / L"packages";
    std::optional<std::string> best;
    std::error_code ec;
    for (fs::directory_iterator it(pkgs, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        if (!it->is_directory(e2)) continue;
        const std::string name = narrow(it->path().filename().wstring());
        if (!speech_version_ok(name) || (best && compare_speech_versions(name, *best) <= 0)) continue;
        if (package_complete(it->path(), name)) best = name;
    }
    if (!best) return make_error("model_missing", "The speech-to-text models are not installed.");
    if (version) *version = *best;
    return pkgs / widen(*best) / L"models" / L"stt";
}

Result<std::string> sha256_file(const fs::path& file, const std::function<bool()>& cancelled) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return make_error("io", "the file cannot be read");
    Sha256 h;
    if (!h.ok()) return make_error("crypto", "SHA-256 is not available");
    std::vector<char> buf(kChunk);
    while (in) {
        if (cancelled && cancelled()) return make_error("cancelled", "the check was cancelled");
        in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        const auto n = static_cast<size_t>(in.gcount());
        if (n > 0 && !h.update(buf.data(), n)) return make_error("crypto", "SHA-256 failed");
    }
    if (in.bad()) return make_error("io", "the file cannot be read");
    return h.finish();
}

Result<std::string> install_speech_archive(const fs::path& data_root, const fs::path& archive, const std::string& expected_sha256,
                                           const SpeechLimits& lim, const std::function<bool()>& cancelled) {
    if (!is_hex64(expected_sha256)) return make_error("speech_digest", "Your administrator has not published a speech package, so this one cannot be verified.");
    std::error_code ec;
    const auto size = fs::file_size(archive, ec);
    if (ec || size == 0 || size > lim.max_archive) return make_error("speech_package", "the speech package has an invalid size");
    auto digest = sha256_file(archive, cancelled);
    if (!digest) return digest.error();
    if (*digest != expected_sha256) return make_error("speech_digest", "the speech package does not match its pinned SHA-256");

    if (auto st = ensure_store(data_root); !st.ok()) return st.error();
    const fs::path staging = speech_root(data_root) / L"staging" / widen("pkg-" + random_urlsafe(9));
    fs::create_directories(staging, ec);
    if (ec) return make_error("io", "the speech staging folder cannot be created");
    Scope scope{staging};

    auto in = std::make_shared<std::ifstream>(archive, std::ios::binary);
    if (!*in) return make_error("io", "the speech package cannot be read");
    SpeechArchive a;
    if (auto st = a.open(file_reader(in), size); !st.ok()) return st.error();
    auto plan = a.inspect(lim, cancelled);
    if (!plan) return plan.error();
    if (auto st = require_space(lim, staging, plan->total); !st.ok()) return st.error();

    // Extract into staging, checking each listed file's digest as its bytes are written.
    for (const auto& e : plan->entries) {
        const fs::path target = staging / widen(e.name);
        fs::create_directories(target.parent_path(), ec);
        if (ec) return make_error("io", "the speech package cannot be extracted");
        std::ofstream out(target, std::ios::binary | std::ios::trunc);
        if (!out) return make_error("io", "the speech package cannot be extracted");
        Sha256 h;
        if (!h.ok()) return make_error("crypto", "SHA-256 is not available");
        bool write_failed = false;
        const std::function<bool(const char*, size_t)> put = [&](const char* p, size_t n) {
            out.write(p, static_cast<std::streamsize>(n));
            if (!out) return write_failed = true, false;
            return h.update(p, n);
        };
        if (auto st = a.extract(e, put, cancelled); !st.ok()) {
            if (write_failed) return make_error("disk_full", "the speech package cannot be written; free disk space and try again");
            return st.error();
        }
        out.close();
        if (out.fail()) return make_error("disk_full", "the speech package cannot be written; free disk space and try again");
        if (e.listed >= 0 && plan->manifest.files[static_cast<size_t>(e.listed)].sha256 != h.finish())
            return make_error("speech_package", "a file does not match the manifest: " + e.name);
    }    for (const char* req : kSpeechRequiredFiles)
        if (!fs::is_regular_file(staging / L"models" / L"stt" / widen(req), ec))
            return make_error("speech_package", std::string("the package lacks ") + req);

    // The completion marker is written before the rename, so a folder under packages\ is complete when it appears.
    const std::string version = plan->manifest.version;
    {
        std::ofstream m(staging / L"complete.json", std::ios::binary | std::ios::trunc);
        m << json{{"schema", kSpeechSchema}, {"package", kSpeechPackageName}, {"architecture", kSpeechArchitecture},
                  {"version", version}, {"archive_sha256", expected_sha256}}
                 .dump();
        m.close();
        if (m.fail()) return make_error("io", "the speech package cannot be finalized");
    }

    const fs::path pkgs = speech_root(data_root) / L"packages";
    fs::create_directories(pkgs, ec);
    const fs::path final_dir = pkgs / widen(version);
    if (fs::is_regular_file(final_dir / L"complete.json", ec)) return version;  // already active; staging is removed
    fs::remove_all(final_dir, ec);  // an unfinished leftover of this version
    fs::rename(staging, final_dir, ec);
    if (ec) return make_error("io", "the speech package cannot be activated: " + ec.message());
    scope.keep = true;
    return version;
}

Result<std::string> download_speech_package(const fs::path& data_root, const SpeechSource& source,
                                            const std::shared_ptr<CancelToken>& cancel,
                                            const std::function<void(std::uint64_t, std::uint64_t)>& progress,
                                            const SpeechLimits& lim, const SpeechTransport& send) {
    if (source.url.empty() || source.sha256.empty())
        return make_error("unavailable", "No speech package is published. Import a package instead.");
    if (!is_https(source.url) || has_userinfo(source.url))
        return make_error("speech_package", "the speech package origin must be an https URL without user information");
    if (!is_hex64(to_lower(source.sha256))) return make_error("speech_digest", "the pinned speech digest is invalid");
    std::set<std::string> hosts;
    for (const auto& h : source.hosts) hosts.insert(to_lower(h));
    hosts.insert(url_host(source.url));
    const std::function<bool()> cancelled = [cancel] { return cancel && cancel->cancelled(); };

    if (auto st = ensure_store(data_root); !st.ok()) return st.error();
    std::error_code ec;
    const fs::path stage = speech_root(data_root) / L"staging" / widen("dl-" + random_urlsafe(9));
    fs::create_directories(stage, ec);
    if (ec) return make_error("io", "the speech staging folder cannot be created");
    Scope scope{stage};


    if (auto st = require_space(lim, stage, 0); !st.ok()) return st.error();
    const fs::path archive = stage / L"package.zip";
    std::ofstream out;
    std::uint64_t done = 0, checked = 0;
    bool too_big = false, write_failed = false;
    std::optional<Error> no_space;
    const auto on_data = [&](std::string_view c) {
        if (!out.is_open()) {
            out.open(archive, std::ios::binary | std::ios::trunc);
            if (!out) return write_failed = true, false;
        }
        if (done + c.size() > lim.max_archive) return too_big = true, false;
        // The volume is rechecked every 8 MB, so the download stops before it takes the reserve.
        if (done + c.size() - checked >= (8u << 20) || checked == 0) {
            checked = done + c.size();
            if (auto st = require_space(lim, stage, c.size()); !st.ok()) return no_space = st.error(), false;
        }
        out.write(c.data(), static_cast<std::streamsize>(c.size()));
        if (!out) return write_failed = true, false;
        done += c.size();
        if (progress) progress(done, 0);
        return true;
    };
    if (auto st = fetch(source.url, hosts, cancel, on_data, send); !st.ok()) {
        if (too_big) return make_error("speech_package", "the speech package is larger than the budget");
        if (no_space) return *no_space;
        if (write_failed) return make_error("disk_full", "the speech package cannot be written; free disk space and try again");
        return st.error();
    }
    if (!out.is_open()) out.open(archive, std::ios::binary | std::ios::trunc);  // an empty body fails the size check below
    out.close();
    if (out.fail()) return make_error("disk_full", "the speech package cannot be written; free disk space and try again");
    return install_speech_archive(data_root, archive, source.sha256, lim, cancelled);
}

Result<bool> remove_speech_version(const fs::path& data_root, const std::string& version, bool in_use) {
    if (!speech_version_ok(version)) return make_error("invalid_params", "unknown speech version");
    const fs::path dir = speech_root(data_root) / L"packages" / widen(version);
    std::error_code ec;
    if (!fs::exists(dir, ec)) return true;
    const auto defer = [&]() -> Result<bool> {
        std::ofstream marker(dir / L".remove-pending", std::ios::binary | std::ios::trunc);
        if (!marker) return make_error("io", "the speech package cannot be marked for removal");
        return false;
    };
    if (in_use) return defer();
    // Out of packages\ first, in one rename: Windows refuses it while a worker has a file of the package open, so a
    // worker that the caller did not report keeps every file. Then the moved folder is deleted.
    const fs::path removing = speech_root(data_root) / L"removing";
    fs::create_directories(removing, ec);
    const fs::path moved = removing / widen(version + "-" + random_urlsafe(6));
    // A scanner or the search indexer opens new files for a moment; a worker keeps its handle. Retry briefly, then defer.
    for (int attempt = 0;; ++attempt) {
        if (::MoveFileExW(dir.c_str(), moved.c_str(), 0)) break;
        const DWORD e = ::GetLastError();
        if (attempt == 9 || (e != ERROR_ACCESS_DENIED && e != ERROR_SHARING_VIOLATION)) return defer();
        ::Sleep(50);
    }
    fs::remove_all(moved, ec);  // anything left is deleted at the next start
    return true;
}

Status verify_speech_version(const fs::path& data_root, const std::string& version, const std::function<bool()>& cancelled) {
    if (!speech_version_ok(version)) return make_error("invalid_params", "unknown speech version");
    const fs::path dir = speech_root(data_root) / L"packages" / widen(version);
    if (!package_complete(dir, version)) return make_error("speech_package", "The speech package is incomplete.");
    std::ifstream in(dir / L"manifest.json", std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    auto manifest = parse_speech_manifest(text);
    if (!manifest || manifest->version != version) return make_error("speech_package", "The speech package manifest is missing or damaged.");
    for (const auto& f : manifest->files) {
        std::error_code ec;
        const fs::path file = dir / widen(f.path);
        if (!fs::is_regular_file(file, ec) || fs::file_size(file, ec) != f.bytes)
            return make_error("speech_package", "A speech file is missing or changed: " + f.path);
        auto digest = sha256_file(file, cancelled);
        if (!digest) return digest.error();
        if (*digest != f.sha256) return make_error("speech_package", "A speech file is damaged: " + f.path);
    }
    return {};
}

std::uint64_t speech_disk_use(const fs::path& data_root) {
    std::uint64_t total = 0;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(speech_root(data_root), fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        if (it->is_regular_file(e2)) total += it->file_size(e2);
    }
    return total;
}

Status recover_speech_store(const fs::path& data_root) {
    const fs::path base = speech_root(data_root);
    std::error_code ec;
    if (!fs::is_directory(base, ec)) return {};
    fs::remove_all(base / L"staging", ec);
    fs::remove_all(base / L"removing", ec);
    std::vector<fs::path> dirs;
    const fs::path pkgs = base / L"packages";
    for (fs::directory_iterator it(pkgs, ec), end; !ec && it != end; it.increment(ec))
        if (it->is_directory()) dirs.push_back(it->path());
    for (const auto& d : dirs) {
        std::error_code e2;
        if (fs::exists(d / L".remove-pending", e2) || !fs::is_regular_file(d / L"complete.json", e2)) fs::remove_all(d, e2);
    }
    return {};
}

}  // namespace wb::meeting