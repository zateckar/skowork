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

constexpr std::uint64_t kMaxManifestBytes = 1u << 20;
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

// A zip file read through miniz's callbacks, so the archive is never loaded whole.
size_t archive_read(void* opaque, mz_uint64 ofs, void* buf, size_t n) {
    auto* in = static_cast<std::ifstream*>(opaque);
    in->clear();
    in->seekg(static_cast<std::streamoff>(ofs));
    if (!*in) return 0;
    in->read(static_cast<char*>(buf), static_cast<std::streamsize>(n));
    return static_cast<size_t>(in->gcount());
}

struct Archive {
    std::ifstream in;
    mz_zip_archive zip{};
    Archive() = default;
    Archive(const Archive&) = delete;
    Archive& operator=(const Archive&) = delete;
    ~Archive() { mz_zip_reader_end(&zip); }
    bool open(const fs::path& p, std::uint64_t size) {
        in.open(p, std::ios::binary);
        if (!in) return false;
        zip.m_pRead = archive_read;
        zip.m_pIO_opaque = &in;
        return mz_zip_reader_init(&zip, size, 0) != 0;
    }
};

struct Entry {
    std::string name;
    mz_uint index = 0;
    std::uint64_t size = 0;
};

Result<std::vector<Entry>> list_entries(Archive& a, const SpeechLimits& lim) {
    const mz_uint n = mz_zip_reader_get_num_files(&a.zip);
    if (n == 0 || n > lim.max_entries) return make_error("speech_package", "the archive has an unsupported number of entries");
    std::vector<Entry> out;
    std::set<std::string> seen;
    std::uint64_t total = 0;
    for (mz_uint i = 0; i < n; ++i) {
        mz_zip_archive_file_stat st{};
        if (!mz_zip_reader_file_stat(&a.zip, i, &st)) return make_error("speech_package", "an archive entry cannot be read");
        Entry e{st.m_filename, i, st.m_uncomp_size};
        if (st.m_is_directory || st.m_is_encrypted) return make_error("speech_package", "the archive has a directory or encrypted entry");
        // Unix file type in the high 16 bits of the external attributes: only regular files (or none) are allowed.
        const mz_uint type = (st.m_external_attr >> 16) & 0170000u;
        if (type != 0 && type != 0100000u) return make_error("speech_package", "the archive has a link or special file: " + e.name);
        if (auto s = check_speech_entry_name(e.name); !s.ok()) return s.error();
        if (e.size > lim.max_entry) return make_error("speech_package", "an archive entry is too large: " + e.name);
        total += e.size;
        if (total > lim.max_total) return make_error("speech_package", "the archive inflates beyond the speech budget");
        if (!seen.insert(to_lower(e.name)).second) return make_error("speech_package", "the archive has a duplicate entry: " + e.name);
        out.push_back(std::move(e));
    }
    return out;
}

struct Sink {
    const std::function<bool(const char*, size_t)>* put = nullptr;
    const std::function<bool()>* cancelled = nullptr;
    std::uint64_t expected = 0, written = 0;
    bool failed = false;
};

size_t sink_write(void* opaque, mz_uint64, const void* buf, size_t n) {
    auto* s = static_cast<Sink*>(opaque);
    if (n == 0) return 0;
    if (s->cancelled && *s->cancelled && (*s->cancelled)()) return s->failed = true, 0;
    // An entry that inflates past its declared size is refused rather than grown.
    if (s->written + n > s->expected || !(*s->put)(static_cast<const char*>(buf), n)) return s->failed = true, 0;
    s->written += n;
    return n;
}

Status extract_entry(Archive& a, const Entry& e, const std::function<bool(const char*, size_t)>& put,
                     const std::function<bool()>& cancelled) {
    Sink s;
    s.put = &put;
    s.cancelled = &cancelled;
    s.expected = e.size;
    const bool ok = mz_zip_reader_extract_to_callback(&a.zip, e.index, sink_write, &s, 0) != 0;
    if (cancelled && cancelled()) return make_error("cancelled", "the speech package was cancelled");
    if (!ok || s.failed || s.written != e.size) return make_error("speech_package", "an archive entry cannot be read: " + e.name);
    return {};
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
             const std::function<bool(std::string_view)>& on_data) {
    std::string url = start;
    for (int hop = 0; hop < 5; ++hop) {
        if (!is_https(url) || !hosts.count(url_host(url)))
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
        auto res = http().send(req);
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

    Archive a;
    if (!a.open(archive, size)) return make_error("speech_package", "the speech package is not a zip archive");
    auto entries = list_entries(a, lim);
    if (!entries) return entries.error();

    const Entry* manifest_entry = nullptr;
    for (const auto& e : *entries)
        if (e.name == "manifest.json") manifest_entry = &e;
    if (!manifest_entry || manifest_entry->size == 0 || manifest_entry->size > kMaxManifestBytes)
        return make_error("speech_package", "the speech package has no usable manifest.json");
    std::string text;
    if (auto st = extract_entry(a, *manifest_entry, [&](const char* p, size_t n) { text.append(p, n); return true; }, cancelled); !st.ok())
        return st.error();
    auto manifest = parse_speech_manifest(text);
    if (!manifest) return manifest.error();

    // Every archive file is either the manifest, a license, or a listed file under its exact path; every listed file is present.
    std::map<std::string, const SpeechManifestFile*> listed;
    for (const auto& f : manifest->files) listed[to_lower(f.path)] = &f;
    size_t matched = 0;
    for (const auto& e : *entries) {
        if (e.name == "manifest.json" || e.name.rfind("licenses/", 0) == 0) continue;
        const auto it = listed.find(to_lower(e.name));
        if (it == listed.end() || it->second->path != e.name)
            return make_error("speech_package", "the archive has a file the manifest does not list: " + e.name);
        ++matched;
    }
    if (matched != manifest->files.size()) return make_error("speech_package", "the manifest lists a file the archive does not contain");

    // Extract into staging, checking each listed file's size and digest as its bytes are written.
    for (const auto& e : *entries) {
        const fs::path target = staging / widen(e.name);
        fs::create_directories(target.parent_path(), ec);
        if (ec) return make_error("io", "the speech package cannot be extracted");
        std::ofstream out(target, std::ios::binary | std::ios::trunc);
        if (!out) return make_error("io", "the speech package cannot be extracted");
        Sha256 h;
        if (!h.ok()) return make_error("crypto", "SHA-256 is not available");
        const std::function<bool(const char*, size_t)> put = [&](const char* p, size_t n) {
            out.write(p, static_cast<std::streamsize>(n));
            return static_cast<bool>(out) && h.update(p, n);
        };
        if (auto st = extract_entry(a, e, put, cancelled); !st.ok()) return st.error();
        out.close();
        if (out.fail()) return make_error("io", "the speech package cannot be written (disk full?)");
        const auto it = listed.find(to_lower(e.name));
        if (it != listed.end() && (it->second->bytes != e.size || it->second->sha256 != h.finish()))
            return make_error("speech_package", "a file does not match the manifest: " + e.name);
    }
    for (const char* req : kSpeechRequiredFiles)
        if (!fs::is_regular_file(staging / L"models" / L"stt" / widen(req), ec))
            return make_error("speech_package", std::string("the package lacks ") + req);

    // The completion marker is written before the rename, so a folder under packages\ is complete when it appears.
    const std::string version = manifest->version;
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
                                            const SpeechLimits& lim) {
    if (source.url.empty() || source.sha256.empty())
        return make_error("unavailable", "No speech package is published. Import a package instead.");
    if (!is_https(source.url)) return make_error("speech_package", "the speech package origin must use https");
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


    const fs::path archive = stage / L"package.zip";
    std::ofstream out;
    std::uint64_t done = 0;
    bool too_big = false;
    const auto on_data = [&](std::string_view c) {
        if (!out.is_open()) {
            out.open(archive, std::ios::binary | std::ios::trunc);
            if (!out) return false;
        }
        if (done + c.size() > lim.max_archive) return too_big = true, false;
        out.write(c.data(), static_cast<std::streamsize>(c.size()));
        if (!out) return false;
        done += c.size();
        if (progress) progress(done, 0);
        return true;
    };
    if (auto st = fetch(source.url, hosts, cancel, on_data); !st.ok()) {
        if (too_big) return make_error("speech_package", "the speech package is larger than the budget");
        return st.error();
    }
    if (!out.is_open()) out.open(archive, std::ios::binary | std::ios::trunc);  // an empty body fails the size check below
    out.close();
    if (out.fail()) return make_error("io", "the speech package cannot be written (disk full?)");
    return install_speech_archive(data_root, archive, source.sha256, lim, cancelled);
}

Result<bool> remove_speech_version(const fs::path& data_root, const std::string& version, bool in_use) {
    if (!speech_version_ok(version)) return make_error("invalid_params", "unknown speech version");
    const fs::path dir = speech_root(data_root) / L"packages" / widen(version);
    std::error_code ec;
    if (!fs::exists(dir, ec)) return true;
    if (in_use) {
        std::ofstream marker(dir / L".remove-pending", std::ios::binary | std::ios::trunc);
        if (!marker) return make_error("io", "the speech package cannot be marked for removal");
        return false;
    }
    fs::remove_all(dir, ec);
    if (ec) return make_error("io", "the speech package cannot be removed: " + ec.message());
    return true;
}

Status recover_speech_store(const fs::path& data_root) {
    const fs::path base = speech_root(data_root);
    std::error_code ec;
    if (!fs::is_directory(base, ec)) return {};
    fs::remove_all(base / L"staging", ec);
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
