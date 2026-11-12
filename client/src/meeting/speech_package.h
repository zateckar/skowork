#pragma once
// client/speech-models (improve-skowork-and-knowledge-control D3, tasks 5.2 and 5.3): acquisition, verification,
// activation, selection and removal of the speech package under the user-private data root:
//   <data root>\speech\staging\<id>\      while a package is being downloaded, checked and extracted
//   <data root>\speech\packages\<version>\ one complete, verified package (models\stt holds the engine's files)
// Nothing here loads a model. The worker probes the selected folder before the client reports speech as ready.
#include "base/result.h"
#include "meeting/speech_manifest.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace wb {
class CancelToken;
}

namespace wb::meeting {


struct SpeechLimits {
    std::uint64_t max_archive = 1536ull << 20;  // the zip as downloaded
    std::uint64_t max_total = 1536ull << 20;    // all entries inflated together
    std::uint64_t max_entry = 1024ull << 20;    // one entry inflated
    size_t max_entries = 64;
};

// Where the package may come from: the catalog of the signed Policy bundle (client/speech-models Speech Package
// Catalog, policy.speech.package.*). The archive's SHA-256 is pinned there, and every redirect must stay on the url's
// host or one of `hosts`. An empty url or digest means no download is offered.
struct SpeechSource {
    std::string url;
    std::string sha256;   // lower-case hex of the whole archive
    std::string version;  // as the administrator named it; informational
    std::vector<std::string> hosts;
};

std::filesystem::path speech_root(const std::filesystem::path& data_root);  // <data root>\speech

// The newest complete, compatible package: <data root>\speech\packages\<version>\models\stt. Fails with model_missing.
Result<std::filesystem::path> active_speech_dir(const std::filesystem::path& data_root, std::string* version = nullptr);

// Installs a local archive (an offline import or a download): checks the archive's SHA-256 against `expected_sha256`,
// extracts it into staging with the entry and size rules, checks every file against the manifest, writes the completion
// marker and renames the folder into packages\<version>. Any failure removes the staging folder and keeps the previous
// package. Returns the installed version. Fails with speech_digest when the digest is not the pinned one.
Result<std::string> install_speech_archive(const std::filesystem::path& data_root, const std::filesystem::path& archive,
                                           const std::string& expected_sha256, const SpeechLimits& limits = {},
                                           const std::function<bool()>& cancelled = {});

// Downloads the archive from `source` without
// credentials, follows redirects only to `source.hosts` over https, then installs it. Progress: (bytes, total or 0).
Result<std::string> download_speech_package(const std::filesystem::path& data_root, const SpeechSource& source,
                                            const std::shared_ptr<CancelToken>& cancel,
                                            const std::function<void(std::uint64_t, std::uint64_t)>& progress = {},
                                            const SpeechLimits& limits = {});

// Removes a version. When `in_use` is true (a capture or transcription uses it) the folder stays and is marked for
// removal at the next start. Returns true when removed now.
Result<bool> remove_speech_version(const std::filesystem::path& data_root, const std::string& version, bool in_use);

// Start-up housekeeping, confined to <data root>\speech: deletes staging, versions without a completion marker and
// versions marked for removal.
Status recover_speech_store(const std::filesystem::path& data_root);

// Lower-case hex SHA-256 of a file, streamed. Cancellation returns "cancelled".
Result<std::string> sha256_file(const std::filesystem::path& file, const std::function<bool()>& cancelled = {});

}  // namespace wb::meeting
