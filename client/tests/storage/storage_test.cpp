// client/local-storage: data root ACL, encrypted databases, migrations with rollback, forget cascade,
// delete everything and the storage quota.
#include "platform/crypto.h"
#include "platform/win.h"
#include "storage/app_store.h"
#include "storage/data_root.h"
#include "storage/database.h"
#include "storage/forget.h"
#include "storage/keystore.h"
#include "storage/quota.h"

#include <gtest/gtest.h>

#include <fstream>
#include <sstream>

using namespace wb;

namespace {

std::filesystem::path temp_root() {
    return std::filesystem::path(local_appdata()) / L"workbench-test" / widen(random_urlsafe(6));
}

std::string slurp(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

void write_file(const std::filesystem::path& p, size_t n) {
    std::filesystem::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary);
    f << std::string(n, 'x');
}

struct TempRoot {
    std::filesystem::path path = temp_root();
    ~TempRoot() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

}  // namespace

TEST(DataRoot, OwnerOnlyAclAndBinding) {
    TempRoot t;
    auto r = DataRoot::open(t.path);
    ASSERT_TRUE(r) << r.error().message;
    std::string detail;
    EXPECT_TRUE(is_private_directory(r->root().wstring(), detail)) << detail;
    EXPECT_TRUE(std::filesystem::exists(r->file("binding.json")));
    EXPECT_NE(slurp(r->file("binding.json")).find(narrow(current_user_sid())), std::string::npos);
    // Reopening as the same user works.
    EXPECT_TRUE(DataRoot::open(t.path));
}

TEST(DataRoot, RefusesARootBoundToAnotherUser) {
    TempRoot t;
    ASSERT_TRUE(DataRoot::open(t.path));
    std::ofstream(t.path / "binding.json", std::ios::trunc) << R"({"windows_sid":"S-1-5-21-1-2-3-1001"})";
    auto r = DataRoot::open(t.path);
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, "other_user");
}

TEST(DataRoot, PolicyRootMustBeInsideTheProfile) {
    EXPECT_TRUE(inside_user_profile(std::filesystem::path(local_appdata()) / L"x"));
    EXPECT_FALSE(inside_user_profile(L"C:\\Windows\\Temp\\x"));
}

TEST(Database, EncryptedAtRestAndKeyBound) {
    TempRoot t;
    auto root = DataRoot::open(t.path);
    ASSERT_TRUE(root);
    KeyStore keys(root->keys());
    ASSERT_TRUE(keys.load());
    auto key = keys.get_or_create("t.db");
    ASSERT_TRUE(key);
    {
        auto db = Database::open(root->file("t.db"), *key);
        ASSERT_TRUE(db) << db.error().message;
        ASSERT_TRUE((*db)->exec("CREATE TABLE t(v TEXT); INSERT INTO t VALUES('plaintext-marker-123');"));
        ASSERT_TRUE((*db)->exec("PRAGMA wal_checkpoint(TRUNCATE);"));
    }
    EXPECT_EQ(slurp(root->file("t.db")).find("plaintext-marker-123"), std::string::npos);
    EXPECT_EQ(slurp(root->file("t.db")).find("SQLite format 3"), std::string::npos);

    db_key wrong = *key;
    wrong[0] ^= 0xFF;
    auto bad = Database::open(root->file("t.db"), wrong);
    ASSERT_FALSE(bad);
    EXPECT_EQ(bad.error().code, "not_a_database");

    // The key file holds DPAPI blobs only: a copy without the user's DPAPI secrets yields no key.
    KeyStore reloaded(root->keys());
    ASSERT_TRUE(reloaded.load());
    auto same = reloaded.get("t.db");
    ASSERT_TRUE(same);
    EXPECT_EQ(*same, *key);
    EXPECT_EQ(slurp(root->keys()).find(std::string(key->begin(), key->end())), std::string::npos);
}

TEST(Database, FailedMigrationRestoresBackupAndOpensReadOnly) {
    TempRoot t;
    auto root = DataRoot::open(t.path);
    ASSERT_TRUE(root);
    KeyStore keys(root->keys());
    ASSERT_TRUE(keys.load());
    auto key = keys.get_or_create("m.db");
    ASSERT_TRUE(key);
    const std::vector<Migration> v1 = {{1, "CREATE TABLE a(x INTEGER); INSERT INTO a VALUES(42);"}};
    {
        MigrationOutcome o;
        auto db = open_and_migrate(root->file("m.db"), *key, v1, o);
        ASSERT_TRUE(db);
        EXPECT_TRUE(o.migrated);
        EXPECT_EQ(o.to_version, 1);
    }
    const std::vector<Migration> v2 = {v1[0], {2, "ALTER TABLE a ADD COLUMN y TEXT; INSERT INTO a VALUES(1, 'x');"},
                                       {3, "THIS IS NOT SQL;"}};
    MigrationOutcome o;
    auto db = open_and_migrate(root->file("m.db"), *key, v2, o);
    ASSERT_TRUE(db) << db.error().message;
    EXPECT_TRUE(o.degraded);
    EXPECT_FALSE(o.failure.empty());
    EXPECT_TRUE((*db)->read_only());
    EXPECT_EQ((*db)->user_version(), 1);  // version 2 rolled back with the restore
    auto s = (*db)->prepare("SELECT COUNT(*), MAX(x) FROM a");
    ASSERT_TRUE(s.step());
    EXPECT_EQ(s.col_int(0), 1);
    EXPECT_EQ(s.col_int(1), 42);
    EXPECT_FALSE((*db)->exec("INSERT INTO a VALUES(7)"));  // read-only
}

TEST(AppStore, SettingsSecretsAndOutbox) {
    TempRoot t;
    auto root = DataRoot::open(t.path);
    ASSERT_TRUE(root);
    KeyStore keys(root->keys());
    ASSERT_TRUE(keys.load());
    auto key = keys.get_or_create("app.db");
    MigrationOutcome o;
    auto db = open_and_migrate(root->file("app.db"), *key, app_migrations(), o);
    ASSERT_TRUE(db);
    AppStore store(std::move(*db));
    ASSERT_TRUE(store.set_setting("a", "1"));
    EXPECT_EQ(store.setting("a"), "1");
    ASSERT_TRUE(store.set_secret("identity.refresh_token", "rt-value"));
    EXPECT_EQ(store.secret("identity.refresh_token"), "rt-value");
    ASSERT_TRUE(store.delete_secret("identity.refresh_token"));
    EXPECT_FALSE(store.secret("identity.refresh_token"));
    ASSERT_TRUE(store.outbox_push("remote-task-result", 7, R"({"ok":true})"));
    auto pending = store.outbox_pending();
    ASSERT_EQ(pending.size(), 1u);
    EXPECT_EQ(pending[0].ref, 7);
    ASSERT_TRUE(store.outbox_remove(pending[0].id));
    EXPECT_TRUE(store.outbox_pending().empty());
    ASSERT_TRUE(store.security_event("state_mismatch", "redirect with unknown state"));
    EXPECT_EQ(store.security_events().size(), 1u);
}

TEST(Forget, RegisteredHandlersRun) {
    ForgetRegistry reg;
    std::vector<std::string> calls;
    reg.on_forget("thread", "inbox", [&](const std::string& id) {
        calls.push_back("inbox:" + id);
        return Status{};
    });
    reg.on_forget("thread", "knowledge", [&](const std::string& id) {
        calls.push_back("knowledge:" + id);
        return Status{};
    });
    ASSERT_TRUE(reg.forget("thread", "t1"));
    EXPECT_EQ(calls, (std::vector<std::string>{"inbox:t1", "knowledge:t1"}));
    EXPECT_EQ(reg.features_for("thread").size(), 2u);
    reg.on_forget("thread", "broken", [](const std::string&) { return Status(make_error("io", "disk")); });
    EXPECT_FALSE(reg.forget("thread", "t2"));
}

TEST(DataRoot, DeleteEverythingReturnsToFirstRun) {
    TempRoot t;
    auto root = DataRoot::open(t.path);
    ASSERT_TRUE(root);
    write_file(root->file("app.db"), 100);
    write_file(root->logs() / "workbench-1.log", 100);
    write_file(root->cache() / "x.bin", 100);
    write_file(root->keys(), 10);
    ASSERT_TRUE(root->wipe());
    std::vector<std::string> left;
    for (const auto& e : std::filesystem::directory_iterator(root->root())) left.push_back(e.path().filename().string());
    // First-run state: the binding, the policy bootstrap and the empty logs and cache folders.
    for (const auto& n : left) EXPECT_TRUE(n == "binding.json" || n == "bootstrap.json" || n == "logs" || n == "cache") << n;
    EXPECT_TRUE(std::filesystem::is_empty(root->logs()));
    EXPECT_TRUE(std::filesystem::is_empty(root->cache()));
    EXPECT_FALSE(std::filesystem::exists(root->keys()));
    std::string detail;
    EXPECT_TRUE(is_private_directory(root->root().wstring(), detail));
}

TEST(Quota, WarningAndIndexingPauseAtNinetyPercent) {
    TempRoot t;
    auto root = DataRoot::open(t.path);
    ASSERT_TRUE(root);
    write_file(root->file("app.db"), 500 * 1024);
    write_file(root->cache() / "c.bin", 100 * 1024);
    StorageQuota quota(1024 * 1024);
    StorageUsage usage;
    int cleared = 0;
    EXPECT_EQ(quota.check(root->root(), [&] { ++cleared; }, usage), QuotaState::normal);
    EXPECT_FALSE(quota.indexing_paused());
    EXPECT_GE(usage.by_category["databases"], 500u * 1024u);

    write_file(root->file("kg.db"), 450 * 1024);  // about 93% even after the cache is cleared
    const QuotaState s = quota.check(root->root(), [&] {
        ++cleared;
        std::filesystem::remove_all(root->cache());
    }, usage);
    EXPECT_EQ(cleared, 1);  // caches are cleared first
    EXPECT_NE(s, QuotaState::normal);
    EXPECT_TRUE(quota.indexing_paused());
    EXPECT_GE(usage.by_category["knowledge"], 400u * 1024u);
}
