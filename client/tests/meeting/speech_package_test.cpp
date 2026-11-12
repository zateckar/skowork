// client/speech-models (tasks 5.2 and 5.3): archive and manifest safety, the pinned digest, activation that keeps the
// previous package, version selection, deferred removal and start-up cleanup. Fixtures are small archives built in the
// test; nothing touches the network or a real model.
#include "base/encoding.h"
#include "base/json.h"
#include "meeting/speech_manifest.h"
#include "meeting/speech_package.h"
#include "platform/crypto.h"
#include "platform/win.h"

#include <miniz.h>

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <iterator>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

namespace wb::meeting {
namespace {

namespace fs = std::filesystem;

using Entries = std::vector<std::pair<std::string, std::string>>;  // archive entry name -> bytes

std::string sha_hex(const std::string& s) { return hex_encode(sha256(std::string_view(s))); }

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

TEST(SpeechPackage, BuildWithoutAPinnedSourceOffersNoDownload) {
    Root root;
    const auto r = download_speech_package(root.path, SpeechSource{}, nullptr);
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, "unavailable");
}

}  // namespace wb::meeting
