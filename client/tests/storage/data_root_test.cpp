// Managed data root and policy.storage.root transitions (client/local-storage Data Location In User Profile).
// Everything runs in a temporary "profile"; the real %LOCALAPPDATA%\Skowork is never touched.
#include "base/json.h"
#include "platform/crypto.h"
#include "platform/win.h"
#include "storage/data_root.h"
#include "storage/keystore.h"
#include "storage/withdrawal_outbox.h"
#include "support/junction.h"

#include <gtest/gtest.h>

#include <fstream>
#include <sstream>

using namespace wb;
namespace fs = std::filesystem;

namespace {

std::string slurp(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

void put(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary);
    f << text;
}

struct Profile {
    fs::path dir = fs::path(local_appdata()) / L"skowork-test" / widen(random_urlsafe(6)) / L"profile";
    RootLocations at{dir / L"AppData" / L"Skowork", dir};
    fs::path custom = dir / L"Data" / L"Skowork";
    Profile() { fs::create_directories(dir); }
    ~Profile() {
        std::error_code ec;
        fs::remove_all(dir.parent_path(), ec);
    }
    // A profile with content worth keeping: a database key, a pending removal, nested files.
    std::string seed(const DataRoot& r) const {
        KeyStore keys(r.keys());
        EXPECT_TRUE(keys.load());
        auto key = keys.get_or_create("app.db");
        EXPECT_TRUE(key);
        WithdrawalState pending;
        pending.entries.push_back({"https://issuer", "https://cp", "subject", "item", "remote-1", 3});
        EXPECT_TRUE(WithdrawalOutbox(r.file("withdrawals.dpapi")).save(pending));
        put(r.file("app.db"), "threads");
        put(r.root() / L"capture" / L"m1" / L"mic.skau", std::string(200'000, 'a'));
        put(r.root() / L"skills" / L"private" / L"s" / L"SKILL.md", "skill");
        return key ? std::string(key->begin(), key->end()) : std::string{};
    }
    void expect_seeded(const DataRoot& r, const std::string& key) const {
        KeyStore keys(r.keys());
        ASSERT_TRUE(keys.load());
        auto k = keys.get_or_create("app.db");
        ASSERT_TRUE(k);
        EXPECT_EQ(std::string(k->begin(), k->end()), key);
        auto pending = WithdrawalOutbox(r.file("withdrawals.dpapi")).load();
        ASSERT_TRUE(pending);
        ASSERT_EQ(pending->entries.size(), 1u);
        EXPECT_EQ(pending->entries[0].remote_id, "remote-1");
        EXPECT_EQ(slurp(r.file("app.db")), "threads");
        EXPECT_EQ(slurp(r.root() / L"capture" / L"m1" / L"mic.skau"), std::string(200'000, 'a'));
        EXPECT_EQ(slurp(r.root() / L"skills" / L"private" / L"s" / L"SKILL.md"), "skill");
        EXPECT_NE(slurp(r.file("binding.json")).find(narrow(current_user_sid())), std::string::npos);
        std::string detail;
        EXPECT_TRUE(is_private_directory(r.root().wstring(), detail)) << detail;
    }
    // Only the anchor's record is left of the default root once the profile has moved away.
    void expect_only_anchor_record() const {
        std::vector<std::string> names;
        for (const auto& e : fs::directory_iterator(at.anchor)) names.push_back(e.path().filename().string());
        EXPECT_EQ(names, std::vector<std::string>{"bootstrap.json"});
    }
    fs::path staging() const { return custom.parent_path() / L"Skowork.skowork-staging"; }
};

}  // namespace

TEST(DataRootTransition, DefaultRootWithoutPolicy) {
    Profile p;
    auto r = DataRoot::open_managed(p.at);
    ASSERT_TRUE(r) << r.error().message;
    EXPECT_EQ(r->root(), p.at.anchor);
    EXPECT_TRUE(r->transition_note().empty());
    EXPECT_TRUE(fs::exists(r->logs()));
}

TEST(DataRootTransition, PolicyRootMovesTheWholeProfileAtTheNextStartAndBack) {
    Profile p;
    std::string key;
    {
        auto r = DataRoot::open_managed(p.at);
        ASSERT_TRUE(r) << r.error().message;
        key = p.seed(*r);
        ASSERT_TRUE(DataRoot::set_requested_root(p.at, p.custom));
        EXPECT_EQ(r->root(), p.at.anchor);  // takes effect on next start
    }
    auto moved = DataRoot::open_managed(p.at);
    ASSERT_TRUE(moved) << moved.error().message;
    EXPECT_EQ(moved->root(), p.custom);
    EXPECT_TRUE(moved->transition_note().empty()) << moved->transition_note();
    p.expect_seeded(*moved, key);
    p.expect_only_anchor_record();
    EXPECT_FALSE(fs::exists(p.staging()));

    // The same request again changes nothing; clearing the policy moves the profile back into the default root.
    ASSERT_TRUE(DataRoot::set_requested_root(p.at, p.custom));
    EXPECT_EQ(DataRoot::open_managed(p.at)->root(), p.custom);
    ASSERT_TRUE(DataRoot::set_requested_root(p.at, std::nullopt));
    auto back = DataRoot::open_managed(p.at);
    ASSERT_TRUE(back) << back.error().message;
    EXPECT_EQ(back->root(), p.at.anchor);
    p.expect_seeded(*back, key);
    EXPECT_FALSE(fs::exists(p.custom));
}

TEST(DataRootTransition, UnsafeOrOccupiedDestinationsAreRefusedAndTheProfileStays) {
    Profile p;
    std::string key;
    {
        auto r = DataRoot::open_managed(p.at);
        ASSERT_TRUE(r);
        key = p.seed(*r);
    }
    // Outside the profile or relative: refused when policy is recorded.
    EXPECT_FALSE(DataRoot::set_requested_root(p.at, p.dir.parent_path() / L"elsewhere"));
    EXPECT_FALSE(DataRoot::set_requested_root(p.at, fs::path(L"relative\\Skowork")));

    const auto refused = [&](const fs::path& target, const std::string& why) {
        ASSERT_TRUE(DataRoot::set_requested_root(p.at, target));
        auto r = DataRoot::open_managed(p.at);
        ASSERT_TRUE(r) << r.error().message;
        EXPECT_EQ(r->root(), p.at.anchor) << why;
        EXPECT_FALSE(r->transition_note().empty()) << why;
        p.expect_seeded(*r, key);
    };
    // An unrelated profile (or anything else) already in the destination.
    put(p.custom / L"binding.json", R"({"windows_sid":"S-1-5-21-other"})");
    refused(p.custom, "occupied");
    EXPECT_EQ(slurp(p.custom / L"binding.json"), R"({"windows_sid":"S-1-5-21-other"})");
    // Nested inside the current root.
    refused(p.at.anchor / L"inner", "nested");
    // Reached through a junction that leads outside the profile.
    const auto outside = p.dir.parent_path() / L"outside";
    fs::create_directories(outside);
    ASSERT_TRUE(test::make_junction(p.dir / L"Link", fs::absolute(outside).wstring()));
    refused(p.dir / L"Link" / L"Skowork", "junction");
    EXPECT_TRUE(fs::is_empty(outside));
    // A link inside the current profile is not followed or copied: the transition is refused.
    ASSERT_TRUE(test::make_junction(p.at.anchor / L"cache" / L"jump", fs::absolute(outside).wstring()));
    refused(p.dir / L"Fresh" / L"Skowork", "link in source");
    EXPECT_FALSE(fs::exists(p.dir / L"Fresh" / L"Skowork"));
    ::RemoveDirectoryW((p.at.anchor / L"cache" / L"jump").c_str());
    ::RemoveDirectoryW((p.dir / L"Link").c_str());
}

TEST(DataRootTransition, InterruptionAtEveryStageRecoversWithoutAPartialProfile) {
    for (const auto point : {TransitionPoint::staged, TransitionPoint::moved_one, TransitionPoint::switched}) {
        Profile p;
        std::string key;
        {
            auto r = DataRoot::open_managed(p.at);
            ASSERT_TRUE(r);
            key = p.seed(*r);
            ASSERT_TRUE(DataRoot::set_requested_root(p.at, p.custom));
        }
        auto stopped = DataRoot::open_managed(p.at, [&](TransitionPoint at) { return at == point; });
        ASSERT_FALSE(stopped);
        EXPECT_EQ(stopped.error().code, "interrupted");
        if (point == TransitionPoint::staged) {
            EXPECT_TRUE(fs::exists(p.staging()));
            EXPECT_FALSE(fs::exists(p.custom));
        }
        // The next start finishes (or redoes) the transition; nothing is lost and nothing is left behind.
        auto r = DataRoot::open_managed(p.at);
        ASSERT_TRUE(r) << r.error().message;
        EXPECT_EQ(r->root(), p.custom) << static_cast<int>(point);
        EXPECT_TRUE(r->transition_note().empty()) << r->transition_note();
        p.expect_seeded(*r, key);
        p.expect_only_anchor_record();
        EXPECT_FALSE(fs::exists(p.staging()));
        const auto boot = parse_json(slurp(p.at.anchor / L"bootstrap.json"));
        ASSERT_TRUE(boot);
        EXPECT_FALSE(boot->contains("transition"));
    }
}

TEST(DataRootTransition, ForgetDataLeavesNoRecoverableCopyOfAnInterruptedTransition) {
    Profile p;
    {
        auto r = DataRoot::open_managed(p.at);
        ASSERT_TRUE(r);
        p.seed(*r);
        ASSERT_TRUE(DataRoot::set_requested_root(p.at, p.custom));
    }
    // Stopped after the commit point: the new root is active, the old copy has not been removed yet.
    ASSERT_FALSE(DataRoot::open_managed(p.at, [](TransitionPoint at) { return at == TransitionPoint::switched; }));
    ASSERT_TRUE(fs::exists(p.at.anchor / L"app.db"));
    // Opening with cleanup blocked by a held file keeps the record; Forget Data must still remove the old copy.
    {
        unique_file locked(::CreateFileW((p.at.anchor / L"app.db").c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr));
        ASSERT_TRUE(locked);
        auto r = DataRoot::open_managed(p.at);
        ASSERT_TRUE(r) << r.error().message;
        EXPECT_EQ(r->root(), p.custom);
        EXPECT_FALSE(r->transition_note().empty());
        locked.reset();
        ASSERT_TRUE(r->wipe()) << "wipe";
        EXPECT_FALSE(fs::exists(r->file("app.db")));
        EXPECT_TRUE(fs::exists(r->file("binding.json")));
    }
    EXPECT_FALSE(fs::exists(p.at.anchor / L"app.db"));
    EXPECT_FALSE(fs::exists(p.at.anchor / L"capture"));
    p.expect_only_anchor_record();
    auto again = DataRoot::open_managed(p.at);
    ASSERT_TRUE(again);
    EXPECT_EQ(again->root(), p.custom);
    EXPECT_FALSE(fs::exists(again->file("app.db")));
}

TEST(DataRootTransition, AMoveThatCannotFinishIsUndoneAndTheSourceStaysCurrent) {
    Profile p;
    std::string key;
    {
        auto r = DataRoot::open_managed(p.at);
        ASSERT_TRUE(r);
        key = p.seed(*r);
        ASSERT_TRUE(DataRoot::set_requested_root(p.at, p.custom));
    }
    ASSERT_FALSE(DataRoot::open_managed(p.at, [](TransitionPoint at) { return at == TransitionPoint::moved_one; }));
    // Something unrelated takes the name of an entry that has not been moved yet.
    fs::path blocked;
    for (const auto& e : fs::directory_iterator(p.staging())) blocked = p.custom / e.path().filename();
    ASSERT_FALSE(blocked.empty());
    put(blocked, "unrelated");

    auto r = DataRoot::open_managed(p.at);
    ASSERT_TRUE(r) << r.error().message;
    EXPECT_EQ(r->root(), p.at.anchor);
    EXPECT_FALSE(r->transition_note().empty());
    p.expect_seeded(*r, key);
    EXPECT_FALSE(fs::exists(p.staging()));
    EXPECT_EQ(slurp(blocked), "unrelated");
    std::vector<fs::path> left;
    for (const auto& e : fs::directory_iterator(p.custom)) left.push_back(e.path());
    EXPECT_EQ(left, std::vector<fs::path>{blocked});
    const auto boot = parse_json(slurp(p.at.anchor / L"bootstrap.json"));
    ASSERT_TRUE(boot);
    EXPECT_FALSE(boot->contains("transition"));

    // Data written to the source afterwards is carried by the next transition, not lost to a stale staged copy.
    put(r->file("app.db"), "threads, later");
    fs::remove(blocked);
    auto moved = DataRoot::open_managed(p.at);
    ASSERT_TRUE(moved) << moved.error().message;
    EXPECT_EQ(moved->root(), p.custom);
    EXPECT_EQ(slurp(moved->file("app.db")), "threads, later");
}