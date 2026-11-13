// The data root (client/local-storage): one folder in the user's profile, owner-only ACL, bound to the Windows
// user that created it (client/identity User Change Detection).
#pragma once

#include "base/result.h"

#include <filesystem>
#include <optional>
#include <string>

namespace wb {

class DataRoot {
public:
    // Resolves the root: `override_root` (tests), else the policy.storage.root pointer stored in the default
    // location's bootstrap file, else %LOCALAPPDATA%\Skowork. Creates it with the owner-only ACL and checks that it
    // is bound to the current Windows user. Error code "other_user" when it belongs to someone else.
    static Result<DataRoot> open(const std::optional<std::filesystem::path>& override_root = std::nullopt);

    [[nodiscard]] const std::filesystem::path& root() const { return root_; }
    [[nodiscard]] std::filesystem::path file(const std::string& name) const { return root_ / name; }
    [[nodiscard]] std::filesystem::path logs() const { return root_ / "logs"; }
    [[nodiscard]] std::filesystem::path cache() const { return root_ / "cache"; }
    [[nodiscard]] std::filesystem::path keys() const { return root_ / "keys.bin"; }

    // policy.storage.root: remembered in the default root's bootstrap file and used from the next start. The
    // folder must be inside the user's profile.
    static Status set_policy_root(const std::optional<std::filesystem::path>& root);
    static std::filesystem::path default_root();

    // Deletes everything under the root except the minimal configuration needed to start (the binding file), so
    // that the next start is a first run (client/local-storage Forget Data, "delete everything").
    Status wipe() const;

private:
    std::filesystem::path root_;
};

// True when `p` is inside the current user's profile folder.
bool inside_user_profile(const std::filesystem::path& p);

}  // namespace wb
