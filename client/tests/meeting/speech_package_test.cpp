// client/speech-models (tasks 5.2 and 5.3): archive and manifest safety, the pinned digest, activation that keeps the
// previous package, version selection, deferred removal and start-up cleanup. Fixtures are small archives built in the
// test; nothing touches the network or a real model.
#include "base/encoding.h"
#include "base/json.h"
#include "meeting/speech_archive.h"
#include "meeting/speech_manifest.h"
#include "meeting/speech_installer.h"
#include "meeting/speech_package.h"
#include "platform/crypto.h"
#include "platform/win.h"

#include <miniz.h>

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <map>
#include <mutex>
#include <thread>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

namespace wb::meeting {
namespace {

namespace fs = std::filesystem;

using Entries = std::vector<std::pair<std::string, std::string>>;  // archive entry name -> bytes

std::string sha_hex(const std::string& s) { return hex_encode(sha256(std::string_view(s))); }

std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// A scratch data root, removed at the end of the test.
struct Root {
    fs::path path;
    Root() {
        path = fs::temp_directory_path() / (L"speech-test-" + std::to_wstring(::GetCurrentProcessId()) + L"-" +
                                            std::to_wstring(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(path);
    }
    ~Root() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

// The complete set of required files with recognizable content, and the manifest that describes exactly that content.
Entries good_package(const std::string& version) {
    Entries entries;
    json files = json::array();
    for (const char* rel : kSpeechRequiredFiles) {
        const std::string content = std::string("content of ") + rel;
        const std::string source = std::string(rel).find(".dll") != std::string::npos ? "runtime" : "models";
        files.push_back({{"path", std::string("models/stt/") + rel}, {"bytes", content.size()}, {"sha256", sha_hex(content)}, {"source", source}});
        entries.emplace_back(std::string("models/stt/") + rel, content);
    }
    const json manifest{{"schema", kSpeechSchema}, {"package", kSpeechPackageName}, {"architecture", kSpeechArchitecture},
                        {"version", version}, {"files", files}};
    entries.insert(entries.begin(), {"manifest.json", manifest.dump()});
    return entries;
}

std::string zip_of(const Entries& entries) {
    mz_zip_archive zip{};
    EXPECT_TRUE(mz_zip_writer_init_heap(&zip, 0, 0));
    for (const auto& [name, data] : entries)
        EXPECT_TRUE(mz_zip_writer_add_mem(&zip, name.c_str(), data.data(), data.size(), 0)) << name;
    void* buf = nullptr;
    size_t size = 0;
    EXPECT_TRUE(mz_zip_writer_finalize_heap_archive(&zip, &buf, &size));
    std::string out(static_cast<const char*>(buf), size);
    mz_zip_writer_end(&zip);
    std::free(buf);
    return out;
}

// Writes the archive into the scratch root and installs it under the digest of its bytes (or `digest` when given).
Result<std::string> install(const Root& root, const Entries& entries, const SpeechLimits& limits = {}, std::string digest = {},
                            const std::function<bool()>& cancelled = {}) {
    static int n = 0;
    const std::string bytes = zip_of(entries);
    const fs::path archive = root.path / ("incoming-" + std::to_string(++n) + ".zip");
    {
        std::ofstream out(archive, std::ios::binary);
        out << bytes;
    }
    if (digest.empty()) digest = sha_hex(bytes);
    return install_speech_archive(root.path, archive, digest, limits, cancelled);
}

std::string active(const Root& root) {
    std::string version;
    return active_speech_dir(root.path, &version) ? version : std::string("(none)");
}

std::string& content_of(Entries& e, const std::string& name) {
    for (auto& p : e)
        if (p.first == name) return p.second;
    ADD_FAILURE() << "no entry " << name;
    static std::string none;
    return none;
}

}  // namespace

TEST(SpeechPackage, EntryNamesAcceptOnlyTheLayoutTheManifestDescribes) {
    for (const char* ok : {"manifest.json", "licenses/CC-BY-4.0.txt", "models/stt/a/b.onnx", "models/stt/silero_vad.onnx"})
        EXPECT_TRUE(check_speech_entry_name(ok).ok()) << ok;
    for (const char* bad : {"../x", "/abs", "models\\stt\\x", "C:x", "models/stt/../x", "models/stt//x", "models/other/x", "models/stt/",
                            "licenses/a/b", "models/stt/a b.onnx", "models/stt/./x", "x\x01y", ""})
        EXPECT_FALSE(check_speech_entry_name(bad).ok()) << bad;
}

TEST(SpeechPackage, ManifestRequiresEveryEngineFileAndValidDigests) {
    const auto good = good_package("1");
    const auto parsed = parse_speech_manifest(good.front().second);
    ASSERT_TRUE(parsed) << parsed.error().message;
    EXPECT_EQ(parsed->version, "1");
    EXPECT_EQ(parsed->files.size(), std::size(kSpeechRequiredFiles));

    json j = json::parse(good.front().second);
    j["files"].erase(j["files"].begin());  // drop one required file
    EXPECT_FALSE(parse_speech_manifest(j.dump()));
    j = json::parse(good.front().second);
    j["files"][0]["sha256"] = std::string(64, 'A');  // upper-case digests are not accepted
    EXPECT_FALSE(parse_speech_manifest(j.dump()));
    j = json::parse(good.front().second);
    j["schema"] = 2;
    EXPECT_FALSE(parse_speech_manifest(j.dump()));
    j = json::parse(good.front().second);
    j["files"][0]["path"] = "../models/stt/escape";
    EXPECT_FALSE(parse_speech_manifest(j.dump()));
    EXPECT_FALSE(parse_speech_manifest("not json"));
}

TEST(SpeechPackage, VersionsCompareNumerically) {
    EXPECT_GT(compare_speech_versions("10", "9"), 0);
    EXPECT_LT(compare_speech_versions("1.2", "1.10"), 0);
    EXPECT_EQ(compare_speech_versions("1", "1"), 0);
    EXPECT_GT(compare_speech_versions("2", "1.9"), 0);
    EXPECT_FALSE(speech_version_ok("../1"));
    EXPECT_FALSE(speech_version_ok(""));
    EXPECT_TRUE(speech_version_ok("1.0-rc_2"));
}

TEST(SpeechPackage, InstallActivatesTheNewestCompletePackage) {
    Root root;
    ASSERT_TRUE(install(root, good_package("1"))) ;
    EXPECT_EQ(active(root), "1");
    ASSERT_TRUE(install(root, good_package("2")));
    EXPECT_EQ(active(root), "2");
    const auto dir = active_speech_dir(root.path);
    ASSERT_TRUE(dir);
    EXPECT_TRUE(fs::is_regular_file(*dir / L"sherpa-onnx-c-api.dll"));
    EXPECT_TRUE(fs::is_regular_file(*dir / L"parakeet-tdt-0.6b-v3-int8" / L"tokens.txt"));
}

TEST(SpeechPackage, ADigestOtherThanThePinnedOneIsRefusedBeforeExtraction) {
    Root root;
    const auto r = install(root, good_package("1"), {}, std::string(64, '0'));
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, "speech_digest");
    EXPECT_EQ(active(root), "(none)");
    EXPECT_FALSE(fs::exists(speech_root(root.path) / L"packages"));
}

TEST(SpeechPackage, AFileThatDiffersFromTheManifestKeepsThePreviousPackage) {
    Root root;
    ASSERT_TRUE(install(root, good_package("1")));
    auto bad = good_package("2");
    content_of(bad, "models/stt/silero_vad.onnx")[0] ^= 1;  // same size, different bytes
    const auto r = install(root, bad);
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, "speech_package");
    EXPECT_EQ(active(root), "1");
    EXPECT_FALSE(fs::exists(speech_root(root.path) / L"packages" / L"2"));
}

TEST(SpeechPackage, TraversalLinksAndUnlistedFilesAreRefused) {
    Root root;
    ASSERT_TRUE(install(root, good_package("1")));
    auto traversal = good_package("2");
    traversal.emplace_back("models/stt/../escape.txt", "x");
    ASSERT_FALSE(install(root, traversal));
    auto extra = good_package("2");
    extra.emplace_back("models/stt/extra.bin", "unlisted");
    const auto r = install(root, extra);
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, "speech_package");
    EXPECT_FALSE(fs::exists(root.path / L"escape.txt"));
    EXPECT_EQ(active(root), "1");
}

TEST(SpeechPackage, EntriesBeyondTheSizeBudgetAreRefused) {
    Root root;
    SpeechLimits tiny;
    tiny.max_entry = 4;  // every model file is longer than this
    ASSERT_FALSE(install(root, good_package("1"), tiny));
    SpeechLimits few;
    few.max_entries = 3;
    ASSERT_FALSE(install(root, good_package("1"), few));
    EXPECT_EQ(active(root), "(none)");
}

TEST(SpeechPackage, CancellationLeavesNoPackageAndNoStaging) {
    Root root;
    const auto r = install(root, good_package("1"), {}, {}, [] { return true; });
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, "cancelled");
    EXPECT_EQ(active(root), "(none)");
    EXPECT_FALSE(fs::exists(speech_root(root.path) / L"packages"));
    std::error_code ec;
    if (fs::exists(speech_root(root.path) / L"staging", ec)) {
        EXPECT_TRUE(fs::is_empty(speech_root(root.path) / L"staging", ec));
    }
}

TEST(SpeechPackage, RemovalOfAVersionInUseIsDeferredToTheNextStart) {
    Root root;
    ASSERT_TRUE(install(root, good_package("1")));
    const auto deferred = remove_speech_version(root.path, "1", true);
    ASSERT_TRUE(deferred);
    EXPECT_FALSE(*deferred);
    EXPECT_EQ(active(root), "1");  // still complete and in place while in use
    const auto removed = remove_speech_version(root.path, "1", false);
    ASSERT_TRUE(removed);
    EXPECT_TRUE(*removed);
    EXPECT_EQ(active(root), "(none)");

    ASSERT_TRUE(install(root, good_package("2")));
    ASSERT_TRUE(remove_speech_version(root.path, "2", true));
    EXPECT_EQ(active(root), "2");
    ASSERT_TRUE(recover_speech_store(root.path));
    EXPECT_EQ(active(root), "(none)");
}

TEST(SpeechPackage, StartUpRemovesStagingAndIncompleteVersionsOnly) {
    Root root;
    ASSERT_TRUE(install(root, good_package("1")));
    fs::create_directories(speech_root(root.path) / L"staging" / L"interrupted");
    fs::create_directories(speech_root(root.path) / L"packages" / L"9" / L"models" / L"stt");  // no completion marker
    ASSERT_TRUE(recover_speech_store(root.path));
    EXPECT_FALSE(fs::exists(speech_root(root.path) / L"staging"));
    EXPECT_FALSE(fs::exists(speech_root(root.path) / L"packages" / L"9"));
    EXPECT_EQ(active(root), "1");
}

// A real https download with a redirect to another host (a 0.6 MB upstream file, not a speech package). Runs only with
// SKOWORK_NET_TESTS=1. The file is fetched and hashed in full, then refused because its digest is not the catalog's.
TEST(SpeechPackage, RealDownloadFollowsOnlyListedRedirectHostsAndChecksTheDigest) {
    if (::GetEnvironmentVariableW(L"SKOWORK_NET_TESTS", nullptr, 0) == 0) GTEST_SKIP() << "set SKOWORK_NET_TESTS=1";
    Root root;
    SpeechSource s;
    s.url = "https://github.com/k2-fsa/sherpa-onnx/releases/download/asr-models/silero_vad.onnx";
    s.sha256 = std::string(64, '0');
    auto r = download_speech_package(root.path, s, nullptr);
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, "speech_redirect") << r.error().message;  // the storage host is not listed

    s.hosts = {"release-assets.githubusercontent.com", "objects.githubusercontent.com"};
    std::uint64_t bytes = 0;
    r = download_speech_package(root.path, s, nullptr, [&](std::uint64_t done, std::uint64_t) { bytes = done; });
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, "speech_digest") << r.error().message;
    EXPECT_GT(bytes, 500000u);
    EXPECT_FALSE(fs::exists(speech_root(root.path) / L"packages" / L"1"));
}

namespace {

// A scripted package origin in place of WinHTTP: each URL answers with a status, a Location or a body that is streamed
// in chunks. Every request is recorded so that tests can see where it went and what it carried.
struct FakeOrigin {
    struct Answer {
        int status = 200;
        std::string location;
        std::string body;
    };
    std::map<std::string, Answer> answers;
    std::vector<HttpRequest> requests;
    size_t chunk = 64 * 1024;
    std::function<void(size_t sent)> after_chunk;
    SpeechTransport transport() {
        return [this](const HttpRequest& req) -> Result<HttpResponse> {
            requests.push_back(req);
            const auto it = answers.find(req.url);
            if (it == answers.end()) return make_error("connect", "no such origin");
            HttpResponse res;
            res.status = it->second.status;
            if (!it->second.location.empty()) res.headers["location"] = it->second.location;
            if (res.status < 200 || res.status > 299 || !req.on_chunk) return res;
            for (size_t at = 0; at < it->second.body.size(); at += chunk) {
                if (req.cancel && req.cancel->cancelled()) return make_error("cancelled", "cancelled");
                if (!req.on_chunk(std::string_view(it->second.body).substr(at, chunk))) return make_error("aborted", "aborted");
                if (after_chunk) after_chunk(at + chunk);
            }
            return res;
        };
    }
};

// Sets the external attributes of one entry in a finished archive's central directory.
std::string with_external_attr(std::string zip, const std::string& name, std::uint32_t attr) {
    for (size_t at = zip.find("PK\x01\x02"); at != std::string::npos; at = zip.find("PK\x01\x02", at + 4)) {
        const auto u16 = [&](size_t o) { return static_cast<size_t>(static_cast<unsigned char>(zip[at + o]) | (static_cast<unsigned char>(zip[at + o + 1]) << 8)); };
        if (zip.compare(at + 46, u16(28), name) != 0 || u16(28) != name.size()) continue;
        for (int b = 0; b < 4; ++b) zip[at + 38 + static_cast<size_t>(b)] = static_cast<char>((attr >> (8 * b)) & 0xff);
        return zip;
    }
    ADD_FAILURE() << "no central entry " << name;
    return zip;
}

bool staging_empty(const Root& root) {    std::error_code ec;
    const auto dir = speech_root(root.path) / L"staging";
    return !fs::exists(dir, ec) || fs::is_empty(dir, ec);
}

}  // namespace

TEST(SpeechPackage, DownloadFollowsAnApprovedRedirectWithoutCredentialsAndActivates) {
    Root root;
    FakeOrigin origin;
    const std::string zip = zip_of(good_package("3"));
    origin.answers["https://cp.example/speech/3.zip"] = {302, "https://store.example/blob/abc", {}};
    origin.answers["https://store.example/blob/abc"] = {200, {}, zip};
    SpeechSource s{"https://cp.example/speech/3.zip", sha_hex(zip), "3", {"store.example"}};
    std::uint64_t bytes = 0;
    const auto r = download_speech_package(root.path, s, nullptr, [&](std::uint64_t done, std::uint64_t) { bytes = done; }, {},
                                           origin.transport());
    ASSERT_TRUE(r) << r.error().message;
    EXPECT_EQ(*r, "3");
    EXPECT_EQ(active(root), "3");
    EXPECT_EQ(bytes, zip.size());
    ASSERT_EQ(origin.requests.size(), 2u);
    for (const auto& req : origin.requests) {
        EXPECT_TRUE(req.headers.empty()) << req.url;  // no bearer token, cookie or other credential
        EXPECT_FALSE(req.follow_redirects) << "each hop is checked here, not followed by WinHTTP";
        EXPECT_FALSE(req.allow_plain_http);
    }
    EXPECT_TRUE(staging_empty(root));
}

TEST(SpeechPackage, RedirectsOutsideTheCatalogOriginsAndUrlCredentialsStopTheDownload) {
    Root root;
    ASSERT_TRUE(install(root, good_package("1")));
    const std::string zip = zip_of(good_package("2"));
    for (const std::string& target : {std::string("https://elsewhere.example/x"), std::string("http://store.example/x"),
                                      std::string("https://user:secret@store.example/x"), std::string("ftp://store.example/x")}) {
        FakeOrigin origin;
        origin.answers["https://cp.example/2.zip"] = {307, target, {}};
        origin.answers[target] = {200, {}, zip};
        const auto r = download_speech_package(root.path, {"https://cp.example/2.zip", sha_hex(zip), "2", {"store.example"}}, nullptr,
                                               {}, {}, origin.transport());
        ASSERT_FALSE(r) << target;
        EXPECT_EQ(r.error().code, "speech_redirect") << target;
        EXPECT_EQ(origin.requests.size(), 1u) << target << ": the redirect target is never contacted";
    }
    FakeOrigin origin;
    const auto r = download_speech_package(root.path, {"https://user:secret@cp.example/2.zip", sha_hex(zip), "2", {}}, nullptr, {}, {},
                                           origin.transport());
    ASSERT_FALSE(r);
    EXPECT_TRUE(origin.requests.empty());
    EXPECT_EQ(active(root), "1");
    EXPECT_TRUE(staging_empty(root));
}

TEST(SpeechPackage, CancellingADownloadLeavesNothingAndARetrySucceeds) {
    Root root;
    FakeOrigin origin;
    const std::string zip = zip_of(good_package("4"));
    origin.chunk = 256;
    origin.answers["https://cp.example/4.zip"] = {200, {}, zip};
    const SpeechSource s{"https://cp.example/4.zip", sha_hex(zip), "4", {}};
    auto cancel = std::make_shared<CancelToken>();
    origin.after_chunk = [&](size_t sent) {
        if (sent >= 512) cancel->cancel();
    };
    auto r = download_speech_package(root.path, s, cancel, {}, {}, origin.transport());
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, "cancelled");
    EXPECT_EQ(active(root), "(none)");
    EXPECT_TRUE(staging_empty(root));

    origin.after_chunk = nullptr;
    r = download_speech_package(root.path, s, std::make_shared<CancelToken>(), {}, {}, origin.transport());
    ASSERT_TRUE(r) << r.error().message;
    EXPECT_EQ(active(root), "4");
}

TEST(SpeechPackage, ADownloadInterruptedByARestartIsDiscardedAtTheNextStart) {
    Root root;
    ASSERT_TRUE(install(root, good_package("1")));
    // What a client that stopped mid-download leaves: a staging folder with part of the archive.
    const std::string zip = zip_of(good_package("2"));
    const auto partial = speech_root(root.path) / L"staging" / L"dl-interrupted";
    fs::create_directories(partial);
    {
        std::ofstream out(partial / L"package.zip", std::ios::binary);
        out << zip.substr(0, zip.size() / 2);
    }
    ASSERT_TRUE(recover_speech_store(root.path));
    EXPECT_FALSE(fs::exists(partial));
    EXPECT_EQ(active(root), "1");
    FakeOrigin origin;
    origin.answers["https://cp.example/2.zip"] = {200, {}, zip};
    const auto r = download_speech_package(root.path, {"https://cp.example/2.zip", sha_hex(zip), "2", {}}, nullptr, {}, {},
                                           origin.transport());
    ASSERT_TRUE(r) << r.error().message;
    EXPECT_EQ(active(root), "2");
}

TEST(SpeechPackage, DiskExhaustionStopsSetupWithAnActionableErrorAndKeepsThePreviousPackage) {
    Root root;
    ASSERT_TRUE(install(root, good_package("1")));
    const std::string zip = zip_of(good_package("2"));
    FakeOrigin origin;
    origin.answers["https://cp.example/2.zip"] = {200, {}, zip};
    const SpeechSource s{"https://cp.example/2.zip", sha_hex(zip), "2", {}};

    // Too little free space before the download: nothing is requested.
    SpeechLimits full;
    full.free_space = [](const fs::path&) { return kSpeechDiskReserve - 1; };
    auto r = download_speech_package(root.path, s, nullptr, {}, full, origin.transport());
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, "disk_full");
    EXPECT_NE(r.error().message.find("MB"), std::string::npos);
    EXPECT_TRUE(origin.requests.empty());

    // The volume fills during a large download: it stops at the next check.
    FakeOrigin big;
    big.chunk = 1u << 20;
    big.answers["https://cp.example/big.zip"] = {200, {}, std::string(20u << 20, 'z')};
    std::uint64_t received = 0;
    SpeechLimits filling;
    filling.free_space = [&](const fs::path&) { return received < (8u << 20) ? (4ull << 30) : kSpeechDiskReserve; };
    r = download_speech_package(root.path, {"https://cp.example/big.zip", std::string(64, 'a'), "9", {}}, nullptr,
                                [&](std::uint64_t done, std::uint64_t) { received = done; }, filling, big.transport());
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, "disk_full");
    EXPECT_LT(received, 20u << 20);

    // Enough for the archive but not for its inflated files: refused before extraction.
    SpeechLimits tight;
    tight.free_space = [](const fs::path&) { return kSpeechDiskReserve + 16; };
    const auto i = install(root, good_package("2"), tight);
    ASSERT_FALSE(i);
    EXPECT_EQ(i.error().code, "disk_full");
    EXPECT_EQ(active(root), "1");
    EXPECT_FALSE(fs::exists(speech_root(root.path) / L"packages" / L"2"));
    EXPECT_TRUE(staging_empty(root));
}

TEST(SpeechPackage, AnIncompatiblePackageIsRefusedAndNamesTheRequiredCompatibility) {
    Root root;
    auto other = good_package("2");
    json m = json::parse(other.front().second);
    m["architecture"] = "windows-arm64";
    other.front().second = m.dump();
    auto r = install(root, other);
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, "speech_incompatible");
    EXPECT_NE(r.error().message.find("schema 1 for windows-x64"), std::string::npos) << r.error().message;
    EXPECT_NE(r.error().message.find("windows-arm64"), std::string::npos);
    m["architecture"] = kSpeechArchitecture;
    m["schema"] = 2;
    other.front().second = m.dump();
    r = install(root, other);
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, "speech_incompatible");
    EXPECT_EQ(active(root), "(none)");
}

TEST(SpeechPackage, ArchiveReaderRefusesCaseDuplicatesWindowsLinksAndSizeMismatches) {
    const auto inspect = [](const std::string& bytes) {
        SpeechArchive a;
        auto s = a.open([&bytes](std::uint64_t ofs, void* buf, size_t n) -> size_t {
            if (ofs >= bytes.size()) return 0;
            n = std::min<size_t>(n, bytes.size() - static_cast<size_t>(ofs));
            std::memcpy(buf, bytes.data() + ofs, n);
            return n;
        }, bytes.size());
        return s.ok() ? a.inspect({}) : Result<SpeechArchivePlan>(s.error());
    };
    const auto ok = inspect(zip_of(good_package("1")));
    ASSERT_TRUE(ok) << ok.error().message;
    EXPECT_EQ(ok->entries.size(), std::size(kSpeechRequiredFiles) + 1);

    auto dup = good_package("1");
    dup.emplace_back("LICENSES/x.txt", "a");
    dup.emplace_back("licenses/X.txt", "b");
    EXPECT_FALSE(inspect(zip_of(dup)));

    auto sized = good_package("1");
    content_of(sized, "models/stt/silero_vad.onnx") += "!";  // one byte more than the manifest says
    EXPECT_FALSE(inspect(zip_of(sized)));

    // A symbolic link from a Unix archiver and an entry a Windows archiver marked as a reparse point.
    const std::string base = zip_of(good_package("1"));
    EXPECT_FALSE(inspect(with_external_attr(base, "models/stt/silero_vad.onnx", 0120777u << 16)));
    EXPECT_FALSE(inspect(with_external_attr(base, "models/stt/silero_vad.onnx", 0x400u)));
    EXPECT_TRUE(inspect(with_external_attr(base, "models/stt/silero_vad.onnx", (0100644u << 16) | 0x20u)));    EXPECT_FALSE(inspect("not a zip"));
}

TEST(SpeechPackage, RemovalKeepsEveryFileWhileAWorkerHoldsOneOpen) {
    Root root;
    ASSERT_TRUE(install(root, good_package("1")));
    const auto dir = active_speech_dir(root.path);
    ASSERT_TRUE(dir);
    {
        // What a running worker holds: the engine DLL, opened without delete sharing as the loader does.
        unique_file held(::CreateFileW((*dir / L"sherpa-onnx-c-api.dll").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                       OPEN_EXISTING, 0, nullptr));
        ASSERT_TRUE(held);
        const auto r = remove_speech_version(root.path, "1", false);  // the caller does not know about this worker
        ASSERT_TRUE(r) << r.error().message;
        EXPECT_FALSE(*r);
        EXPECT_EQ(active(root), "1");
        EXPECT_TRUE(verify_speech_version(root.path, "1").ok()) << "no file of the package was deleted";
    }
    ASSERT_TRUE(recover_speech_store(root.path));  // the next start
    EXPECT_EQ(active(root), "(none)");
    EXPECT_FALSE(fs::exists(speech_root(root.path) / L"packages" / L"1"));
    EXPECT_FALSE(fs::exists(speech_root(root.path) / L"removing"));

    ASSERT_TRUE(install(root, good_package("2")));
    const auto now = remove_speech_version(root.path, "2", false);
    ASSERT_TRUE(now);
    EXPECT_TRUE(*now);
    EXPECT_FALSE(fs::exists(speech_root(root.path) / L"packages" / L"2"));
}

TEST(SpeechPackage, VerifyNamesADamagedOrMissingFileAndDiskUseCountsThePackage) {
    Root root;
    ASSERT_TRUE(install(root, good_package("1")));
    EXPECT_TRUE(verify_speech_version(root.path, "1").ok());
    std::uint64_t files = 0;
    for (const auto& [name, data] : good_package("1")) files += data.size();
    EXPECT_GE(speech_disk_use(root.path), files);
    const auto dir = *active_speech_dir(root.path);
    {
        std::fstream f(dir / L"silero_vad.onnx", std::ios::binary | std::ios::in | std::ios::out);
        f.seekp(0);
        f.put('#');
    }
    auto s = verify_speech_version(root.path, "1");
    ASSERT_FALSE(s.ok());
    EXPECT_NE(s.error().message.find("silero_vad.onnx"), std::string::npos) << s.error().message;
    fs::remove(dir / L"onnxruntime.dll");
    s = verify_speech_version(root.path, "1");
    ASSERT_FALSE(s.ok());
    EXPECT_FALSE(verify_speech_version(root.path, "9").ok());
}

namespace {

// Drives a SpeechInstaller with a scripted origin and probe, and waits for a setup to settle.
struct Installer {
    Root root;
    FakeOrigin origin;
    std::function<Status(const fs::path&)> probe = [](const fs::path&) { return Status{}; };
    bool in_use = false;
    int activated = 0;
    std::vector<std::string> states;
    std::mutex mx;
    SpeechSource source;
    std::unique_ptr<SpeechInstaller> svc;
    Installer() {
        SpeechInstaller::Env env;
        env.data_root = root.path;
        env.source = [this] { return source; };
        env.probe = [this](const fs::path& d) { return probe(d); };
        env.in_use = [this] { return in_use; };
        env.activated = [this] { ++activated; };
        env.changed = [this](const json& e) {
            std::lock_guard lk(mx);
            states.push_back(jstr(e, "state"));
        };
        env.send = origin.transport();
        svc = std::make_unique<SpeechInstaller>(std::move(env));
    }
    void publish(const std::string& version) {
        const std::string zip = zip_of(good_package(version));
        origin.answers["https://cp.example/" + version + ".zip"] = {200, {}, zip};
        source = {"https://cp.example/" + version + ".zip", sha_hex(zip), version, {}};
    }
    json settle() {
        json s;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        do {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            s = svc->status();
        } while ((jstr(s, "state") == "downloading" || jstr(s, "state") == "verifying" || jstr(s, "state") == "probing") &&
                 std::chrono::steady_clock::now() < deadline);
        return s;
    }
};

}  // namespace

TEST(SpeechInstaller, ASetupIsReadyOnlyAfterTheProbeAndRefreshesAvailabilityAtOnce) {
    Installer f;
    f.publish("1");
    std::vector<fs::path> probed;
    f.probe = [&](const fs::path& d) { probed.push_back(d); return Status{}; };
    ASSERT_TRUE(f.svc->start_download());
    auto s = f.settle();
    EXPECT_EQ(jstr(s, "state"), "ready") << s.dump();
    EXPECT_EQ(jstr(s, "version"), "1");
    EXPECT_GT(jint(s, "disk_bytes"), 0);
    EXPECT_EQ(f.activated, 1);
    ASSERT_EQ(probed.size(), 1u);
    EXPECT_EQ(probed[0], *active_speech_dir(f.root.path));

    // A newer package whose engine fails: the previous one stays active and availability is not refreshed.
    f.publish("2");
    f.probe = [](const fs::path& d) {
        return d.parent_path().parent_path().filename() == L"2" ? Status(make_error("engine", "cannot start")) : Status{};
    };
    ASSERT_TRUE(f.svc->start_download());
    s = f.settle();
    EXPECT_EQ(jstr(s, "state"), "failed");
    EXPECT_EQ(jstr(s, "version"), "1");
    EXPECT_EQ(active(f.root), "1");
    EXPECT_FALSE(fs::exists(speech_root(f.root.path) / L"packages" / L"2"));
    EXPECT_EQ(f.activated, 1);
}

TEST(SpeechInstaller, VerifyChecksFilesAndTheEngineWithoutChangingThePackage) {
    Installer f;
    f.publish("1");
    ASSERT_FALSE(f.svc->start_verify());  // nothing installed
    ASSERT_TRUE(f.svc->start_download());
    ASSERT_EQ(jstr(f.settle(), "state"), "ready");
    ASSERT_TRUE(f.svc->start_verify());
    EXPECT_EQ(jstr(f.settle(), "state"), "ready");
    const auto dir = *active_speech_dir(f.root.path);
    {
        std::ofstream(dir / L"silero_vad.onnx", std::ios::binary | std::ios::trunc) << "damaged";
    }
    ASSERT_TRUE(f.svc->start_verify());
    const auto s = f.settle();
    EXPECT_EQ(jstr(s, "state"), "failed");
    EXPECT_NE(jstr(s, "error").find("silero_vad.onnx"), std::string::npos) << s.dump();
    EXPECT_EQ(active(f.root), "1") << "verify reports, it does not remove";
}

TEST(SpeechInstaller, RepairReplacesADamagedPackageButNeverWhileItIsInUse) {
    Installer f;
    f.publish("1");
    ASSERT_TRUE(f.svc->start_download());
    ASSERT_EQ(jstr(f.settle(), "state"), "ready");
    const auto dir = *active_speech_dir(f.root.path);
    {
        std::ofstream(dir / L"silero_vad.onnx", std::ios::binary | std::ios::trunc) << "damaged";
    }
    // A transcription uses the package: Repair explains and touches nothing.
    f.in_use = true;
    const auto busy = f.svc->start_repair();
    ASSERT_FALSE(busy);
    EXPECT_EQ(busy.error().code, "in_use");
    EXPECT_EQ(slurp(dir / L"silero_vad.onnx"), "damaged");

    f.in_use = false;
    ASSERT_TRUE(f.svc->start_repair());
    auto s = f.settle();
    EXPECT_EQ(jstr(s, "state"), "ready") << s.dump();
    EXPECT_TRUE(verify_speech_version(f.root.path, "1").ok());

    // Without a download, a damaged package is taken out of use and Import is asked for.
    {
        std::ofstream(*active_speech_dir(f.root.path) / L"silero_vad.onnx", std::ios::binary | std::ios::trunc) << "damaged";
    }
    f.source = {};
    ASSERT_TRUE(f.svc->start_repair());
    s = f.settle();
    EXPECT_EQ(jstr(s, "state"), "failed");
    EXPECT_NE(jstr(s, "error").find("Import"), std::string::npos) << s.dump();
    EXPECT_EQ(active(f.root), "(none)");
}
// The fuzz seeds stay meaningful: the valid ones are accepted packages, the others are refused for their named reason.
TEST(SpeechPackage, FuzzSeedArchivesAreJudgedAsNamed) {
    const fs::path corpus = widen(SKOWORK_SPEECH_CORPUS);
    size_t seen = 0;
    for (const auto& f : fs::directory_iterator(corpus)) {
        std::ifstream in(f.path(), std::ios::binary);
        const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        SpeechArchive a;
        ASSERT_TRUE(a.open([&bytes](std::uint64_t ofs, void* buf, size_t n) -> size_t {
            if (ofs >= bytes.size()) return 0;
            n = std::min<size_t>(n, bytes.size() - static_cast<size_t>(ofs));
            std::memcpy(buf, bytes.data() + ofs, n);
            return n;
        }, bytes.size()).ok());
        const auto plan = a.inspect({});
        const bool valid = f.path().filename().string().rfind("valid", 0) == 0;
        EXPECT_EQ(static_cast<bool>(plan), valid) << f.path().filename().string() << (plan ? "" : ": " + plan.error().message);
        ++seen;
    }
    EXPECT_GE(seen, 5u);
}

TEST(SpeechPackage, BuildWithoutAPinnedSourceOffersNoDownload) {
    Root root;
    const auto r = download_speech_package(root.path, SpeechSource{}, nullptr);
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, "unavailable");
}

}  // namespace wb::meeting
