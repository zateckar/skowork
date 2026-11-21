// Private snapshot of the Teams IndexedDB LevelDB folder (client/teams; add-local-outlook-and-teams design D4
// "Location", "Snapshot"). Source files are opened with read, write and delete sharing so Teams is never blocked; the
// LOCK file is never opened and nothing in the source folder is written.
#pragma once

#include <filesystem>
#include <vector>

#include "base/result.h"

namespace wb::teams {

// Copies CURRENT, the MANIFEST it names, the live tables (all *.ldb/*.sst when the manifest cannot be read) and every
// *.log into private_dir, reusing a table already copied under the same name and size and deleting stale copies.
// A file that disappears while being copied restarts the snapshot once. Returns private_dir.
// Errors: "not_found" (no CURRENT/MANIFEST), "corrupt" (CURRENT names no manifest), "vanished" (a file vanished
// twice), "io".
Result<std::filesystem::path> snapshot_cache(const std::filesystem::path& leveldb_dir,
                                             const std::filesystem::path& private_dir);

// Existing Packages/MSTeams_8wekyb3d8bbwe/LocalCache/Microsoft/MSTeams/EBWebView/WV2Profile_*/IndexedDB/
// https_teams.microsoft.com_0.indexeddb.leveldb folders under local_app_data, most recently modified first.
std::vector<std::filesystem::path> find_teams_profiles(const std::filesystem::path& local_app_data);

}  // namespace wb::teams
