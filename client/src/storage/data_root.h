// The data root (client/local-storage): one folder in the user's profile, owner-only ACL, bound to the Windows
// user that created it (client/identity User Change Detection).
#pragma once

#include "base/result.h"

#include <filesystem>
#include <functional>
#include <optional>
#include <string>

namespace wb {

// Where the anchor (the default root, holding bootstrap.json) and the user's profile are. Production uses
// DataRoot::default_locations(); tests point both at temporary folders.
struct RootLocations {
    std::filesystem::path anchor;
    std::filesystem::path profile;
};

// Stage boundaries of a root transition; a test hook returning true stops the transition there, as if the process
// had ended (design D2 root transition).
enum class TransitionPoint { staged, moved_one, switched };
using TransitionStop = std::function<bool(TransitionPoint)>;

class DataRoot {
public:
    // Resolves the root: `override_root` (tests), else the managed root selected by policy.storage.root. Creates it
    // with the owner-only ACL and checks that it is bound to the current Windows user. Error code "other_user" when
    // it belongs to someone else.
    static Result<DataRoot> open(const std::optional<std::filesystem::path>& override_root = std::nullopt);

    // The managed root: completes or recovers a recorded transition, moves the profile to the requested root when it
    // differs from the active one, and opens the active root. Nothing else may have the root open.
    static Result<DataRoot> open_managed(const RootLocations& at, const TransitionStop& stop = {});

    [[nodiscard]] const std::filesystem::path& root() const { return root_; }
    [[nodiscard]] std::filesystem::path file(const std::string& name) const { return root_ / name; }
    [[nodiscard]] std::filesystem::path logs() const { return root_ / "logs"; }
    [[nodiscard]] std::filesystem::path cache() const { return root_ / "cache"; }
    [[nodiscard]] std::filesystem::path keys() const { return root_ / "keys.bin"; }
    // Why a requested root transition was refused or could not finish at this start; empty when none. Reported once
    // logging has started.
    [[nodiscard]] const std::string& transition_note() const { return transition_note_; }

    // policy.storage.root: records the requested root in the anchor; the transition happens at the next start. An
    // empty value requests the default root. The folder must be inside the user's profile.
    static Status set_policy_root(const std::optional<std::filesystem::path>& root);
    static Status set_requested_root(const RootLocations& at, const std::optional<std::filesystem::path>& root);
    static std::filesystem::path default_root();
    static RootLocations default_locations();

    // Deletes everything under the root except the minimal configuration needed to start (the binding file), so
    // that the next start is a first run (client/local-storage Forget Data, "delete everything"). A recorded root
    // transition is discarded first, so neither its staging copy nor the inactive root can restore content.
    Status wipe() const;

private:
    std::filesystem::path root_;
    std::optional<RootLocations> managed_;
    std::string transition_note_;
};

// True when `p` is inside the current user's profile folder.
bool inside_user_profile(const std::filesystem::path& p);

}  // namespace wb
