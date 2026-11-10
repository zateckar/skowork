// client/local-storage: data root ACL, encrypted databases, migrations with rollback, forget cascade,
// delete everything and the storage quota.
#include "platform/crypto.h"
#include "platform/win.h"
#include "knowledge/kg_store.h"
#include "storage/app_store.h"
#include "storage/data_root.h"
#include "storage/database.h"
#include "storage/forget.h"
#include "storage/keystore.h"
#include "storage/quota.h"
#include "storage/withdrawal_outbox.h"

#include <gtest/gtest.h>
#include <sqlite3mc_amalgamation.h>

#include <fstream>
#include <atomic>
#include <chrono>
#include <iostream>
#include <set>
#include <sstream>
#include <thread>

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

TEST(WithdrawalOutbox, EncryptsIdentitiesAndRefusesToOverwriteCorruption) {
    TempRoot t;
    auto root = DataRoot::open(t.path);
    ASSERT_TRUE(root);
    WithdrawalOutbox box(root->file("withdrawals.dpapi"));
    const WithdrawalIdentity identity{"https://identity.example/realm", "https://server.example", "subject-private-123", "item", "k-private-456", 0};
    WithdrawalState state{true, {identity}};
    ASSERT_TRUE(box.save(state));
    const auto raw = slurp(root->file("withdrawals.dpapi"));
    EXPECT_EQ(raw.find(identity.subject), std::string::npos);
    EXPECT_EQ(raw.find(identity.remote_id), std::string::npos);
    const auto loaded = box.load();
    ASSERT_TRUE(loaded);
    ASSERT_TRUE(loaded->wipe_pending);
    ASSERT_EQ(loaded->entries.size(), 1u);
    EXPECT_EQ(loaded->entries[0].subject, identity.subject);
    EXPECT_EQ(loaded->entries[0].remote_id, identity.remote_id);
    EXPECT_FALSE(box.save(state));  // A second writer cannot replace a newer snapshot.
    auto invalid = *loaded;
    invalid.entries[0].remote_id = "../other";
    EXPECT_FALSE(box.save(invalid));
    EXPECT_EQ(slurp(root->file("withdrawals.dpapi")), raw);
    {
        std::ofstream out(root->file("withdrawals.dpapi"), std::ios::binary | std::ios::trunc);
        out << "corrupted";
    }
    EXPECT_FALSE(box.load());
    EXPECT_FALSE(box.save(state));
    EXPECT_EQ(slurp(root->file("withdrawals.dpapi")), "corrupted");
}

TEST(WithdrawalOutbox, ConcurrentWritersPreserveEveryRemoval) {
    TempRoot t;
    auto root = DataRoot::open(t.path);
    ASSERT_TRUE(root);
    const auto file = root->file("withdrawals.dpapi");
    std::atomic<int> errors{0};
    std::mutex errors_mx;
    std::vector<std::string> details;
    const auto record_error = [&](const std::string& message) {
        const std::lock_guard lock(errors_mx);
        details.push_back(message);
    };
    auto append = [&](int writer) {
        WithdrawalOutbox box(file);
        for (int i = 0; i < 12; ++i) {
            bool saved = false;
            for (int retry = 0; retry < 100 && !saved; ++retry) {
                auto state = box.load();
                if (!state) {
                    record_error("load: " + state.error().code + ": " + state.error().message);
                    break;
                }
                state->entries.push_back({"issuer", "endpoint", "subject", "item",
                                          "k-" + std::to_string(writer) + "-" + std::to_string(i), 0});
                const auto result = box.save(*state);
                saved = result.ok();
                if (!result && result.error().code != "stale") {
                    record_error("save: " + result.error().code + ": " + result.error().message);
                    break;
                }
            }
            if (!saved) ++errors;
        }
    };
    std::thread a(append, 1), b(append, 2);
    a.join();
    b.join();
    for (const auto& detail : details) ADD_FAILURE() << detail;
    EXPECT_EQ(errors, 0);
    const auto state = WithdrawalOutbox(file).load();
    ASSERT_TRUE(state);
    EXPECT_EQ(state->entries.size(), 24u);
    EXPECT_EQ(state->revision, 24);
}

// Invoked only by the parent test below, with a disposable profile path in its environment.
TEST(WithdrawalOutbox, ChildProcessAppend) {
    std::wstring path(32768, L'\0');
    const DWORD path_size = ::GetEnvironmentVariableW(L"SKOWORK_TEST_WITHDRAWAL_PATH", path.data(), static_cast<DWORD>(path.size()));
    if (!path_size) GTEST_SKIP() << "Only used by the cross-process test.";
    ASSERT_LT(path_size, path.size());
    path.resize(path_size);
    wchar_t writer[16]{};
    const DWORD writer_size = ::GetEnvironmentVariableW(L"SKOWORK_TEST_WITHDRAWAL_WRITER", writer, 16);
    ASSERT_GT(writer_size, 0u);
    ASSERT_LT(writer_size, 16u);
    WithdrawalOutbox box{std::filesystem::path(path)};
    for (int i = 0; i < 20; ++i) {
        bool saved = false;
        for (int retry = 0; retry < 100 && !saved; ++retry) {
            auto state = box.load();
            ASSERT_TRUE(state) << state.error().message;
            state->entries.push_back({"issuer", "endpoint", "subject", "item",
                                      "child-" + narrow(writer) + "-" + std::to_string(i), 0});
            const auto result = box.save(*state);
            saved = result.ok();
            if (!result && result.error().code != "stale") FAIL() << result.error().message;
        }
        ASSERT_TRUE(saved);
    }
}

TEST(WithdrawalOutbox, SeparateProcessesPreserveEveryRemoval) {
    TempRoot t;
    auto root = DataRoot::open(t.path);
    ASSERT_TRUE(root);
    const auto path = root->file("withdrawals.dpapi");
    wchar_t executable[32768]{};
    const DWORD exe_size = ::GetModuleFileNameW(nullptr, executable, 32768);
    ASSERT_GT(exe_size, 0u);
    ASSERT_LT(exe_size, 32768u);
    struct EnvironmentCleanup {
        ~EnvironmentCleanup() {
            ::SetEnvironmentVariableW(L"SKOWORK_TEST_WITHDRAWAL_PATH", nullptr);
            ::SetEnvironmentVariableW(L"SKOWORK_TEST_WITHDRAWAL_WRITER", nullptr);
        }
    } cleanup;
    ASSERT_TRUE(::SetEnvironmentVariableW(L"SKOWORK_TEST_WITHDRAWAL_PATH", path.c_str()));
    std::vector<unique_handle> children;
    for (const wchar_t* writer : {L"one", L"two"}) {
        ASSERT_TRUE(::SetEnvironmentVariableW(L"SKOWORK_TEST_WITHDRAWAL_WRITER", writer));
        std::wstring command = L"\"" + std::wstring(executable, exe_size) +
                               L"\" --gtest_filter=WithdrawalOutbox.ChildProcessAppend --gtest_brief=1";
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        const BOOL started = ::CreateProcessW(executable, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                                               nullptr, nullptr, &startup, &process);
        if (!started) {
            for (const auto& child : children) { ::TerminateProcess(child.get(), 1); ::WaitForSingleObject(child.get(), 5000); }
            FAIL() << "Could not start the outbox test process: " << ::GetLastError();
        }
        ::CloseHandle(process.hThread);
        children.emplace_back(process.hProcess);
    }
    for (const auto& child : children) {
        if (::WaitForSingleObject(child.get(), 30000) != WAIT_OBJECT_0) {
            ::TerminateProcess(child.get(), 1);
            ::WaitForSingleObject(child.get(), 5000);
            ADD_FAILURE() << "Outbox test process timed out.";
            continue;
        }
        DWORD exit_code = 1;
        ASSERT_TRUE(::GetExitCodeProcess(child.get(), &exit_code));
        EXPECT_EQ(exit_code, 0u);
    }
    const auto state = WithdrawalOutbox(path).load();
    ASSERT_TRUE(state) << state.error().message;
    EXPECT_EQ(state->entries.size(), 40u);
    EXPECT_EQ(state->revision, 40);
    std::set<std::string> ids;
    for (const auto& entry : state->entries) ids.insert(entry.remote_id);
    for (const std::string writer : {"one", "two"})
        for (int i = 0; i < 20; ++i) EXPECT_TRUE(ids.count("child-" + writer + "-" + std::to_string(i)));
}

TEST(WithdrawalOutbox, FailedReplacementPreservesPendingRemovals) {
    TempRoot t;
    auto root = DataRoot::open(t.path);
    ASSERT_TRUE(root);
    const auto file = root->file("withdrawals.dpapi");
    WithdrawalOutbox box(file);
    WithdrawalState state{false, {{"issuer", "endpoint", "subject", "item", "k-first", 0}}};
    ASSERT_TRUE(box.save(state));
    auto next = box.load();
    ASSERT_TRUE(next);
    next->entries.push_back({"issuer", "endpoint", "subject", "item", "k-second", 0});
    const auto previous = slurp(file);
    // Keep a reader open without delete sharing so Windows refuses the replacement.
    unique_file held(::CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    ASSERT_TRUE(held);
    EXPECT_FALSE(box.save(*next));
    EXPECT_EQ(slurp(file), previous);
    held.reset();
    const auto unchanged = box.load();
    ASSERT_TRUE(unchanged);
    ASSERT_EQ(unchanged->entries.size(), 1u);
    EXPECT_EQ(unchanged->entries[0].remote_id, "k-first");
    ASSERT_TRUE(box.save(*next));
    const auto recovered = box.load();
    ASSERT_TRUE(recovered);
    EXPECT_EQ(recovered->entries.size(), 2u);
}

TEST(WithdrawalOutbox, InterruptedWipeCompletesBeforeReopeningTheProfile) {
    TempRoot t;
    auto root = DataRoot::open(t.path);
    ASSERT_TRUE(root);
    std::ofstream(root->file("private-content.txt")) << "private content";
    WithdrawalOutbox box(root->file("withdrawals.dpapi"));
    WithdrawalState state{true, {{"issuer", "endpoint", "subject", "item", "k-remote", 0}}};
    ASSERT_TRUE(box.save(state));
    const auto failed = box.finish_interrupted_wipe([] { return Status(make_error("wipe", "simulated failure")); });
    EXPECT_FALSE(failed);
    EXPECT_TRUE(std::filesystem::exists(root->file("private-content.txt")));
    ASSERT_TRUE(box.load());
    EXPECT_TRUE(box.load()->wipe_pending);
    auto reopened = DataRoot::open(t.path);
    ASSERT_TRUE(reopened);
    EXPECT_FALSE(std::filesystem::exists(root->file("private-content.txt")));
    const auto after = box.load();
    ASSERT_TRUE(after);
    EXPECT_FALSE(after->wipe_pending);
    ASSERT_EQ(after->entries.size(), 1u);
    EXPECT_EQ(after->entries[0].remote_id, "k-remote");
    {
        std::ofstream out(root->file("withdrawals.dpapi"), std::ios::binary | std::ios::trunc);
        out << "corrupted";
    }
    EXPECT_FALSE(DataRoot::open(t.path));
}

TEST(WithdrawalOutbox, RetriesOnlyForTheBoundAccountAndKeepsConcurrentChanges) {
    TempRoot t;
    auto root = DataRoot::open(t.path);
    ASSERT_TRUE(root);
    WithdrawalOutbox box(root->file("withdrawals.dpapi"));
    WithdrawalState state{false, {{"issuer", "endpoint", "alice", "item", "k-alice", 0},
                                  {"issuer", "endpoint", "bob", "item", "k-bob", 0}}};
    ASSERT_TRUE(box.save(state));
    int calls = 0;
    const auto alice_pending = box.has_item("issuer", "endpoint", "alice", "k-alice");
    const auto bob_pending = box.has_item("issuer", "endpoint", "bob", "k-alice");
    ASSERT_TRUE(alice_pending);
    ASSERT_TRUE(bob_pending);
    EXPECT_TRUE(*alice_pending);
    EXPECT_FALSE(*bob_pending);
    auto wrong = box.retry_items("issuer", "other-endpoint", "alice", [&](const std::string&) { ++calls; return Status{}; });
    ASSERT_TRUE(wrong);
    EXPECT_EQ(*wrong, 0u);
    EXPECT_EQ(calls, 0);
    const auto offline = box.retry_items("issuer", "endpoint", "alice", [&](const std::string&) {
        return Status(make_error("offline", "offline"));
    });
    EXPECT_FALSE(offline);
    ASSERT_TRUE(box.load());
    EXPECT_EQ(box.load()->entries.size(), 2u);
    const auto raced = box.retry_items("issuer", "endpoint", "alice", [&](const std::string&) {
        auto other = box.load();
        if (!other) return Status(other.error());
        other->entries.push_back({"issuer", "endpoint", "alice", "item", "k-new", 0});
        return box.save(*other);
    });
    ASSERT_FALSE(raced);
    EXPECT_EQ(raced.error().code, "stale");
    ASSERT_TRUE(box.load());
    EXPECT_EQ(box.load()->entries.size(), 3u);
    auto sent = box.retry_items("issuer", "endpoint", "alice", [&](const std::string& id) {
        EXPECT_NE(id, "k-bob");
        ++calls;
        return Status{};
    });
    ASSERT_TRUE(sent);
    EXPECT_EQ(*sent, 2u);
    const auto remaining = box.load();
    ASSERT_TRUE(remaining);
    ASSERT_EQ(remaining->entries.size(), 1u);
    EXPECT_EQ(remaining->entries[0].subject, "bob");
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

TEST(Database, MigrationRefusesBlockedCheckpointWithoutOverwritingBackup) {
    TempRoot t;
    auto root = DataRoot::open(t.path);
    ASSERT_TRUE(root);
    KeyStore keys(root->keys());
    ASSERT_TRUE(keys.load());
    auto key = keys.get_or_create("busy.db");
    ASSERT_TRUE(key);
    const std::vector<Migration> v1 = {{1, "CREATE TABLE a(x INTEGER); INSERT INTO a VALUES(42);"}};
    MigrationOutcome initial;
    auto writer = open_and_migrate(root->file("busy.db"), *key, v1, initial);
    ASSERT_TRUE(writer);
    auto reader = Database::open(root->file("busy.db"), *key);
    ASSERT_TRUE(reader);
    ASSERT_TRUE((*reader)->exec("BEGIN;"));
    {
        auto snapshot = (*reader)->prepare("SELECT count(*) FROM a");
        ASSERT_TRUE(snapshot.step());
        EXPECT_EQ(snapshot.col_int(0), 1);
    }
    ASSERT_TRUE((*writer)->exec("INSERT INTO a VALUES(99);"));
    const auto backup = root->file("busy.db.bak");
    std::ofstream(backup, std::ios::binary) << "prior backup marker";
    const std::vector<Migration> v2 = {v1[0], {2, "ALTER TABLE a ADD COLUMN y TEXT;"}};
    MigrationOutcome attempted;
    const auto migration = open_and_migrate(root->file("busy.db"), *key, v2, attempted);
    ASSERT_FALSE(migration);
    EXPECT_EQ(migration.error().code, "backup");
    EXPECT_EQ(slurp(backup), "prior backup marker");
    ASSERT_TRUE((*reader)->exec("ROLLBACK;"));
    EXPECT_EQ((*writer)->user_version(), 1);
    auto rows = (*writer)->prepare("SELECT count(*),sum(x) FROM a");
    ASSERT_TRUE(rows.step());
    EXPECT_EQ(rows.col_int(0), 2);
    EXPECT_EQ(rows.col_int(1), 141);
}

TEST(Database, PrototypeEncryptedSnapshotWhileWriterReservationIsHeld) {
    TempRoot t;
    auto root = DataRoot::open(t.path);
    ASSERT_TRUE(root);
    KeyStore keys(root->keys());
    ASSERT_TRUE(keys.load());
    auto key = keys.get_or_create("source.db");
    ASSERT_TRUE(key);
    const std::vector<Migration> v1 = {{1, "CREATE TABLE a(x INTEGER); INSERT INTO a VALUES(42);"}};
    MigrationOutcome initial;
    auto source = open_and_migrate(root->file("source.db"), *key, v1, initial);
    ASSERT_TRUE(source);
    ASSERT_TRUE((*source)->exec("INSERT INTO a VALUES(99);"));
    auto other = Database::open(root->file("source.db"), *key);
    ASSERT_TRUE(other);
    sqlite3_busy_timeout((*other)->raw(), 0);
    auto snapshot = Database::open(root->file("snapshot.db"), *key);
    ASSERT_TRUE(snapshot);
    ASSERT_TRUE((*source)->exec("BEGIN IMMEDIATE;"));
    EXPECT_FALSE((*other)->exec("INSERT INTO a VALUES(7);"));
    // SQLite does not permit the backup source connection itself to be in a write transaction. A separate read
    // connection supplies the snapshot while the first connection holds the reserved writer lock.
    sqlite3_backup* copy = sqlite3_backup_init((*snapshot)->raw(), "main", (*other)->raw(), "main");
    ASSERT_NE(copy, nullptr);
    const int copied = sqlite3_backup_step(copy, -1);
    const int finished = sqlite3_backup_finish(copy);
    EXPECT_EQ(copied, SQLITE_DONE);
    EXPECT_EQ(finished, SQLITE_OK);
    EXPECT_TRUE((*source)->exec("ROLLBACK;"));
    auto rows = (*snapshot)->prepare("SELECT count(*),sum(x) FROM a");
    ASSERT_TRUE(rows.step());
    EXPECT_EQ(rows.col_int(0), 2);
    EXPECT_EQ(rows.col_int(1), 141);
    EXPECT_EQ(slurp(root->file("snapshot.db")).find("SQLite format 3"), std::string::npos);
    db_key wrong = *key;
    wrong[0] ^= 0xff;
    EXPECT_FALSE(Database::open(root->file("snapshot.db"), wrong));
}

TEST(Database, ConsistentKnowledgeMigrationSnapshotsAndRollsBackAllVersions) {
    TempRoot t;
    auto root = DataRoot::open(t.path);
    ASSERT_TRUE(root);
    KeyStore keys(root->keys());
    ASSERT_TRUE(keys.load());
    auto key = keys.get_or_create("consistent.db");
    ASSERT_TRUE(key);
    const std::vector<Migration> v1 = {{1, "CREATE TABLE a(x INTEGER); INSERT INTO a VALUES(42);"}};
    MigrationOutcome initial;
    auto original = open_and_migrate_consistently(root->file("consistent.db"), *key, v1, initial);
    ASSERT_TRUE(original);
    ASSERT_TRUE((*original)->exec("INSERT INTO a VALUES(99);"));
    auto old_writer = Database::open(root->file("consistent.db"), *key);
    ASSERT_TRUE(old_writer);
    sqlite3_busy_timeout((*old_writer)->raw(), 0);
    struct Probe { sqlite3* old_writer; bool blocked = false; } probe{(*old_writer)->raw()};
    const auto configure = [&probe](Database& db) -> Status {
        const int rc = sqlite3_create_function_v2(db.raw(), "test_old_writer", 0, SQLITE_UTF8, &probe,
            [](sqlite3_context* context, int, sqlite3_value**) {
                auto* p = static_cast<Probe*>(sqlite3_user_data(context));
                const int result = sqlite3_exec(p->old_writer, "INSERT INTO a VALUES(7)", nullptr, nullptr, nullptr);
                p->blocked = result == SQLITE_BUSY || result == SQLITE_LOCKED;
                sqlite3_result_int(context, p->blocked ? 1 : 0);
            }, nullptr, nullptr, nullptr);
        return rc == SQLITE_OK ? Status{} : Status(make_error("sqlite", db.errmsg()));
    };
    const std::vector<Migration> bad = {v1[0], {2, "SELECT test_old_writer(); ALTER TABLE a ADD COLUMN y TEXT;"},
                                         {3, "THIS IS NOT SQL;"}};
    original->reset();
    MigrationOutcome failed;
    auto result = open_and_migrate_consistently(root->file("consistent.db"), *key, bad, failed, configure);
    ASSERT_TRUE(result) << result.error().message;
    EXPECT_TRUE(probe.blocked);
    EXPECT_TRUE(failed.degraded);
    EXPECT_FALSE(failed.migrated);
    EXPECT_EQ(failed.to_version, 1);
    EXPECT_TRUE((*result)->read_only());
    auto rows = (*result)->prepare("SELECT count(*),sum(x) FROM a");
    ASSERT_TRUE(rows.step());
    EXPECT_EQ(rows.col_int(0), 2);
    EXPECT_EQ(rows.col_int(1), 141);
    auto backup = Database::open(root->file("consistent.db.bak"), *key, true);
    ASSERT_TRUE(backup);
    EXPECT_EQ((*backup)->user_version(), 1);
    auto backed = (*backup)->prepare("SELECT count(*),sum(x) FROM a");
    ASSERT_TRUE(backed.step());
    EXPECT_EQ(backed.col_int(0), 2);
    EXPECT_EQ(backed.col_int(1), 141);
    EXPECT_EQ(slurp(root->file("consistent.db.bak")).find("SQLite format 3"), std::string::npos);
}

TEST(Database, ConsistentKnowledgeMigrationCommitsLegacyWriterBarrierWithSchema) {
    TempRoot t;
    auto root = DataRoot::open(t.path);
    ASSERT_TRUE(root);
    KeyStore keys(root->keys());
    ASSERT_TRUE(keys.load());
    auto key = keys.get_or_create("knowledge.db");
    ASSERT_TRUE(key);
    MigrationOutcome initial;
    auto original = knowledge::open_kg_database(root->file("knowledge.db"), *key, initial);
    ASSERT_TRUE(original);
    ASSERT_TRUE((*original)->exec("INSERT INTO items(source,ext_id,title) VALUES('files','one','original')"));
    auto legacy = Database::open(root->file("knowledge.db"), *key);
    ASSERT_TRUE(legacy);
    original->reset();
    auto migrations = knowledge::kg_migrations();
    migrations.push_back({knowledge::kKgEvidenceSchemaVersion, "CREATE TABLE evidence_probe(id INTEGER PRIMARY KEY);" + knowledge::kg_writer_barrier_sql()});
    MigrationOutcome updated;
    auto modern = open_and_migrate_consistently(root->file("knowledge.db"), *key, migrations, updated, knowledge::register_kg_writer);
    ASSERT_TRUE(modern) << modern.error().message;
    EXPECT_TRUE(updated.migrated);
    EXPECT_EQ(updated.to_version, knowledge::kKgEvidenceSchemaVersion);
    EXPECT_FALSE((*legacy)->exec("UPDATE items SET title='old writer' WHERE ext_id='one'"));
    ASSERT_TRUE((*modern)->exec("UPDATE items SET title='new writer' WHERE ext_id='one'"));
    auto title = (*modern)->prepare("SELECT title FROM items WHERE ext_id='one'");
    ASSERT_TRUE(title.step());
    EXPECT_EQ(title.col_text(0), "new writer");
    auto backup = Database::open(root->file("knowledge.db.bak"), *key, true);
    ASSERT_TRUE(backup);
    EXPECT_EQ((*backup)->user_version(), knowledge::kKgEvidenceSchemaVersion - 1);
    auto old_title = (*backup)->prepare("SELECT title FROM items WHERE ext_id='one'");
    ASSERT_TRUE(old_title.step());
    EXPECT_EQ(old_title.col_text(0), "original");
}

TEST(Database, EvidenceMigrationPreservesIndependentAndRequiredSupports) {
    TempRoot t;
    auto root = DataRoot::open(t.path);
    ASSERT_TRUE(root);
    KeyStore keys(root->keys());
    ASSERT_TRUE(keys.load());
    auto key = keys.get_or_create("evidence.db");
    ASSERT_TRUE(key);
    MigrationOutcome initial;
    auto original = knowledge::open_kg_database(root->file("evidence.db"), *key, initial);
    ASSERT_TRUE(original);
    auto setup = (*original)->exec(R"sql(
INSERT INTO items(id,source,ext_id,hash) VALUES(1,'files','a','h1'),(2,'files','b','h2'),(3,'notes','3','h3');
INSERT INTO chunks(id,item_id,ord,text) VALUES(1,1,0,'first passage'),(2,2,0,'second passage'),(3,3,0,'note passage');
INSERT INTO entities(id,type,name,key) VALUES(1,'person','A','a'),(2,'person','B','b'),(3,'person','C','c');
INSERT INTO relations(id,subject,predicate,object,confidence,valid_from,chunk_id) VALUES
  (1,1,'works with',2,1,'2025',1),(2,1,'works with',2,1,'2025',1),
  (3,1,'works with',2,1,'2025',2),(4,1,'works with',2,1,'2026',3);
INSERT INTO notes(id,title,text,created_at,updated_at) VALUES(3,'note','note passage',1,1);
INSERT INTO facts_user(id,entity_id,text,created_at,item_source,item_ext_id) VALUES(1,1,'correction',1,'files','a');
INSERT INTO fact_tombstones(source,ext_id,item_hash,fact_key) VALUES('files','a','h1','removed');
)sql");
    ASSERT_TRUE(setup) << setup.error().message;
    ASSERT_TRUE((*original)->exec("PRAGMA wal_checkpoint(TRUNCATE)"));
    original->reset();
    const auto copy = root->file("fixture.db");
    std::filesystem::copy_file(root->file("evidence.db"), copy);
    auto legacy = Database::open(copy, *key);
    ASSERT_TRUE(legacy);
    auto migrations = knowledge::kg_migrations();
    migrations.push_back({knowledge::kKgEvidenceSchemaVersion, knowledge::kg_evidence_migration_sql()});
    MigrationOutcome outcome;
    auto db = open_and_migrate_consistently(copy, *key, migrations, outcome, knowledge::register_kg_writer);
    ASSERT_TRUE(db) << db.error().message;
    ASSERT_FALSE(outcome.degraded) << outcome.failure;
    ASSERT_EQ((*db)->user_version(), knowledge::kKgEvidenceSchemaVersion);
    auto counts = (*db)->prepare(R"sql(SELECT
      (SELECT count(*) FROM knowledge_revisions),
      (SELECT count(*) FROM knowledge_facts),
      (SELECT count(*) FROM knowledge_support_sets),
      (SELECT count(*) FROM knowledge_evidence WHERE precision='passage' AND byte_start IS NULL AND byte_end IS NULL),
      (SELECT count(*) FROM knowledge_annotations WHERE revision_id=(SELECT id FROM knowledge_revisions WHERE item_id=1)),
      (SELECT count(*) FROM notes), (SELECT count(*) FROM fact_tombstones))sql");
    ASSERT_TRUE(counts.step());
    EXPECT_EQ(counts.col_int(0), 3);
    EXPECT_EQ(counts.col_int(1), 2);
    EXPECT_EQ(counts.col_int(2), 3);
    EXPECT_EQ(counts.col_int(3), 3);
    EXPECT_EQ(counts.col_int(4), 1);
    EXPECT_EQ(counts.col_int(5), 1);
    EXPECT_EQ(counts.col_int(6), 1);
    ASSERT_TRUE((*db)->exec("DELETE FROM relations WHERE id=1"));
    {
        auto duplicate = (*db)->prepare("SELECT count(*) FROM knowledge_support_sets WHERE legacy_relation_id=2 AND state='unverified'");
        ASSERT_TRUE(duplicate.step());
        EXPECT_EQ(duplicate.col_int(0), 1);
    }
    EXPECT_FALSE((*legacy)->exec("UPDATE items SET title='legacy' WHERE id=1"));
    EXPECT_FALSE((*db)->exec("INSERT INTO knowledge_evidence(revision_id,excerpt,precision) VALUES(1,'invalid','exact')"));
    EXPECT_FALSE((*db)->exec("INSERT INTO knowledge_evidence(revision_id,chunk_id,excerpt,precision,byte_start,byte_end) VALUES(1,2,'second','exact',0,6)"));
    EXPECT_FALSE((*db)->exec("INSERT INTO knowledge_evidence(revision_id,chunk_id,excerpt,precision,byte_start,byte_end) VALUES(1,1,'made up','exact',0,7)"));
    ASSERT_TRUE((*db)->exec("INSERT INTO knowledge_evidence(revision_id,chunk_id,excerpt,precision,byte_start,byte_end) VALUES(1,1,'first','exact',0,5)"));
    ASSERT_TRUE((*db)->exec(R"sql(
INSERT INTO knowledge_facts(id,subject,predicate,object) VALUES(100,1,'derived',3),(101,2,'derived',3);
INSERT INTO knowledge_support_sets(id,fact_id,state) VALUES(100,100,'current'),(101,100,'current'),(102,101,'current');
INSERT INTO knowledge_support_evidence(support_id,evidence_id)
 SELECT 100,id FROM knowledge_evidence WHERE chunk_id IN (1,2);
INSERT INTO knowledge_support_evidence(support_id,evidence_id)
 SELECT 101,id FROM knowledge_evidence WHERE chunk_id=3;
INSERT INTO knowledge_support_facts(support_id,required_fact_id) VALUES(102,100);
INSERT INTO knowledge_support_sets(id,fact_id,state) VALUES(110,100,'current');
INSERT INTO knowledge_support_evidence(support_id,evidence_id)
 SELECT 110,id FROM knowledge_evidence WHERE chunk_id IN (1,2) AND precision='passage';
DELETE FROM knowledge_support_evidence WHERE support_id=110 AND evidence_id=(SELECT id FROM knowledge_evidence WHERE chunk_id=1 AND precision='passage');
)sql"));
    {
        auto whole_proof = (*db)->prepare("SELECT count(*) FROM knowledge_support_sets WHERE id=110");
        ASSERT_TRUE(whole_proof.step());
        EXPECT_EQ(whole_proof.col_int(0), 0);
    }
    EXPECT_FALSE((*db)->exec("INSERT INTO knowledge_support_facts(support_id,required_fact_id) VALUES(100,101)"));
    EXPECT_FALSE((*db)->exec("UPDATE knowledge_support_facts SET required_fact_id=101 WHERE support_id=102"));
    int before = 0;
    {
        auto generation = (*db)->prepare("SELECT value FROM knowledge_generation WHERE id=1");
        ASSERT_TRUE(generation.step());
        before = generation.col_int(0);
    }
    ASSERT_TRUE((*db)->exec("DELETE FROM items WHERE id=1"));
    auto after = (*db)->prepare(R"sql(SELECT
      (SELECT count(*) FROM knowledge_support_sets WHERE id=100),
      (SELECT count(*) FROM knowledge_support_sets WHERE id=101),
      (SELECT count(*) FROM knowledge_facts WHERE id=100),
      (SELECT count(*) FROM knowledge_support_sets WHERE fact_id=(SELECT id FROM knowledge_facts WHERE predicate='works with' AND valid_from='2025')),
      (SELECT value FROM knowledge_generation WHERE id=1))sql");
    ASSERT_TRUE(after.step());
    EXPECT_EQ(after.col_int(0), 0);
    EXPECT_EQ(after.col_int(1), 1);
    EXPECT_EQ(after.col_int(2), 1);
    EXPECT_EQ(after.col_int(3), 1);
    EXPECT_GT(after.col_int(4), before);
    ASSERT_TRUE((*db)->exec(R"sql(
INSERT INTO knowledge_facts(id,subject,predicate,object) VALUES
 (200,1,'base',3),(201,1,'middle',3),(202,1,'dependent',3);
INSERT INTO knowledge_support_sets(id,fact_id,state) VALUES
 (200,200,'current'),(201,201,'current'),(202,202,'current'),(203,201,'current');
INSERT INTO knowledge_support_facts(support_id,required_fact_id) VALUES(201,200),(202,201);
UPDATE knowledge_support_sets SET state='unsupported' WHERE id=200;
)sql"));
    {
        auto states = (*db)->prepare("SELECT state FROM knowledge_support_sets WHERE id IN (201,202,203) ORDER BY id");
        ASSERT_TRUE(states.step()); EXPECT_EQ(states.col_text(0), "unsupported");
        ASSERT_TRUE(states.step()); EXPECT_EQ(states.col_text(0), "current");
        ASSERT_TRUE(states.step()); EXPECT_EQ(states.col_text(0), "current");
    }
    ASSERT_TRUE((*db)->exec("UPDATE knowledge_support_sets SET state='unsupported' WHERE id=203"));
    {
        auto transitive = (*db)->prepare("SELECT state FROM knowledge_support_sets WHERE id=202");
        ASSERT_TRUE(transitive.step());
        EXPECT_EQ(transitive.col_text(0), "unsupported");
    }
    ASSERT_TRUE((*db)->exec("DELETE FROM items WHERE id=3"));
    auto cascade = (*db)->prepare("SELECT (SELECT count(*) FROM knowledge_facts WHERE id IN (100,101)), "
                                  "(SELECT count(*) FROM knowledge_support_sets WHERE id IN (100,101,102)), "
                                  "(SELECT count(*) FROM knowledge_evidence WHERE excerpt='note passage')");
    ASSERT_TRUE(cascade.step());
    EXPECT_EQ(cascade.col_int(0), 0);
    EXPECT_EQ(cascade.col_int(1), 0);
    EXPECT_EQ(cascade.col_int(2), 0);
}

TEST(Database, EvidenceMigrationFailureRollsBackSchemaAndWriterBarrier) {
    TempRoot t;
    auto root = DataRoot::open(t.path);
    ASSERT_TRUE(root);
    KeyStore keys(root->keys());
    ASSERT_TRUE(keys.load());
    auto key = keys.get_or_create("rollback.db");
    ASSERT_TRUE(key);
    MigrationOutcome initial;
    auto db = knowledge::open_kg_database(root->file("rollback.db"), *key, initial);
    ASSERT_TRUE(db);
    ASSERT_TRUE((*db)->exec("INSERT INTO items(source,ext_id,title) VALUES('files','a','preserved')"));
    db->reset();
    auto migrations = knowledge::kg_migrations();
    migrations.push_back({knowledge::kKgEvidenceSchemaVersion, knowledge::kg_evidence_migration_sql() + "THIS IS NOT SQL;"});
    MigrationOutcome outcome;
    auto failed = open_and_migrate_consistently(root->file("rollback.db"), *key, migrations, outcome, knowledge::register_kg_writer);
    ASSERT_TRUE(failed) << failed.error().message;
    ASSERT_TRUE(outcome.degraded);
    EXPECT_TRUE((*failed)->read_only());
    EXPECT_EQ((*failed)->user_version(), knowledge::kKgEvidenceSchemaVersion - 1);
    auto rows = (*failed)->prepare("SELECT (SELECT count(*) FROM items WHERE title='preserved'), "
                                  "(SELECT count(*) FROM sqlite_master WHERE name='knowledge_revisions' OR name LIKE 'skowork_guard_%')");
    ASSERT_TRUE(rows.step());
    EXPECT_EQ(rows.col_int(0), 1);
    EXPECT_EQ(rows.col_int(1), 0);
    auto old = Database::open(root->file("rollback.db"), *key);
    ASSERT_TRUE(old);
    EXPECT_TRUE((*old)->exec("UPDATE items SET title='legacy still works' WHERE ext_id='a'"));
}

TEST(Database, KnowledgeCategoryDecisionsSurviveSuggestionsAndMerges) {
    TempRoot t;
    auto root = DataRoot::open(t.path);
    ASSERT_TRUE(root);
    KeyStore keys(root->keys());
    ASSERT_TRUE(keys.load());
    auto key = keys.get_or_create("categories.db");
    ASSERT_TRUE(key);
    auto migrations = knowledge::kg_migrations();
    migrations.push_back({knowledge::kKgEvidenceSchemaVersion, knowledge::kg_evidence_migration_sql()});
    MigrationOutcome outcome;
    auto db = open_and_migrate_consistently(root->file("categories.db"), *key, migrations, outcome, knowledge::register_kg_writer);
    ASSERT_TRUE(db) << db.error().message;
    ASSERT_FALSE(outcome.degraded) << outcome.failure;
    knowledge::KgStore store(std::move(*db));
    ASSERT_TRUE(store.db().exec(R"sql(
INSERT INTO entities(id,type,name,key) VALUES(1,'person','A','a'),(2,'person','B','b');
INSERT INTO knowledge_facts(id,subject,predicate,object) VALUES(1,1,'first',2),(2,1,'second',2),(3,1,'third',2);
INSERT INTO items(id,source,ext_id,hash) VALUES(1,'files','category-source','v1');
INSERT INTO chunks(id,item_id,ord,text) VALUES(1,1,0,'evidence');
INSERT INTO knowledge_revisions(id,item_id,sequence,source_hash,indexed_at,current) VALUES(1,1,1,'v1',1,1);
INSERT INTO knowledge_evidence(id,revision_id,chunk_id,excerpt,precision,byte_start,byte_end) VALUES(1,1,1,'evidence','exact',0,8);
INSERT INTO knowledge_support_sets(id,fact_id,state) VALUES(1,1,'current'),(2,2,'current'),(3,3,'current');
INSERT INTO knowledge_support_evidence(support_id,evidence_id) VALUES(1,1),(2,1),(3,1);
)sql"));
    auto sales = store.create_category(" Sales ");
    auto engineering = store.create_category("Engineering");
    auto people = store.create_category("People");
    ASSERT_TRUE(sales);
    ASSERT_TRUE(engineering);
    ASSERT_TRUE(people);
    EXPECT_FALSE(store.create_category("sales"));
    EXPECT_FALSE(store.set_category_assignment(1, *sales, "automatic", "no evidence"));
    EXPECT_FALSE(store.set_category_assignment(1, *sales, "automatic", "invented evidence", 999, store.knowledge_generation()));
    const auto stale_generation = store.knowledge_generation();
    ASSERT_TRUE(stale_generation);
    ASSERT_TRUE(store.db().exec("UPDATE knowledge_facts SET unit='' WHERE id=1"));
    EXPECT_FALSE(store.set_category_assignment(1, *sales, "automatic", "stale", 1, stale_generation));
    ASSERT_TRUE(store.set_category_assignment(1, *sales, "automatic", "model explanation", 1, store.knowledge_generation()));
    ASSERT_TRUE(store.set_category_assignment(1, *sales, "rejected", "user rejected"));
    ASSERT_TRUE(store.set_category_assignment(1, *sales, "automatic", "new suggestion", 1, store.knowledge_generation()));
    auto assignment = store.category_assignments(1);
    ASSERT_EQ(assignment.size(), 1u);
    EXPECT_EQ(assignment[0].decision, "rejected");
    EXPECT_EQ(assignment[0].reason, "user rejected");
    ASSERT_TRUE(store.set_category_assignment(1, *engineering, "manual", "user accepted"));
    ASSERT_TRUE(store.set_category_assignment(2, *sales, "manual", "move me"));
    ASSERT_TRUE(store.set_category_assignment(2, *engineering, "automatic", "old suggestion", 1, store.knowledge_generation()));
    ASSERT_TRUE(store.set_category_assignment(3, *sales, "automatic", "transfer me", 1, store.knowledge_generation()));
    ASSERT_TRUE(store.set_category_assignment(1, *people, "manual", "second category"));
    ASSERT_TRUE(store.merge_categories(*sales, *engineering));
    EXPECT_EQ(store.categories().size(), 2u);
    assignment = store.category_assignments(1);
    ASSERT_EQ(assignment.size(), 2u);
    EXPECT_EQ(assignment[0].decision, "manual");
    EXPECT_EQ(assignment[0].reason, "user accepted");
    assignment = store.category_assignments(2);
    ASSERT_EQ(assignment.size(), 1u);
    EXPECT_EQ(assignment[0].decision, "manual");
    EXPECT_EQ(assignment[0].reason, "move me");
    ASSERT_TRUE(store.rename_category(*engineering, "Research"));
    ASSERT_TRUE(store.set_category_assignment(3, *engineering, "rejected", "not research"));
    auto uncategorized = store.uncategorized_facts(10);
    ASSERT_EQ(uncategorized.size(), 1u);
    EXPECT_EQ(uncategorized[0], 3);
    ASSERT_TRUE(store.reset_category_assignment(3, *engineering));
    ASSERT_TRUE(store.set_category_assignment(3, *engineering, "automatic", "reconsidered", 1, store.knowledge_generation()));
    EXPECT_TRUE(store.uncategorized_facts(10).empty());
    ASSERT_TRUE(store.db().exec(R"sql(
INSERT INTO knowledge_evidence(id,revision_id,chunk_id,excerpt,precision,byte_start,byte_end) VALUES(2,1,1,'evi','exact',0,3);
INSERT INTO knowledge_support_sets(id,fact_id,state) VALUES(4,3,'current');
INSERT INTO knowledge_support_evidence(support_id,evidence_id) VALUES(4,2);
UPDATE knowledge_support_sets SET state='unsupported' WHERE id=3;
)sql"));
    EXPECT_TRUE(store.category_assignments(3).empty());
    EXPECT_EQ(store.uncategorized_facts(10), std::vector<long long>{3});
    ASSERT_TRUE(store.db().exec("UPDATE knowledge_support_sets SET state='unsupported' WHERE id=4"));
    EXPECT_TRUE(store.uncategorized_facts(10).empty());
    ASSERT_TRUE(store.db().exec("UPDATE knowledge_support_sets SET state='current' WHERE id=4"));
    EXPECT_FALSE(store.set_category_assignment(999, *engineering, "manual", ""));
    ASSERT_TRUE(store.delete_category(*engineering));
    auto remaining = store.db().prepare("SELECT count(*) FROM knowledge_facts");
    ASSERT_TRUE(remaining.step());
    EXPECT_EQ(remaining.col_int(0), 3);
    uncategorized = store.uncategorized_facts(10);
    ASSERT_EQ(uncategorized.size(), 2u);
    EXPECT_EQ(uncategorized[0], 2);
    EXPECT_EQ(uncategorized[1], 3);
}

TEST(Database, KnowledgeAnalysisBindsOnlyCurrentSupportedUtf8Excerpts) {
    TempRoot t;
    auto root = DataRoot::open(t.path);
    ASSERT_TRUE(root);
    KeyStore keys(root->keys());
    ASSERT_TRUE(keys.load());
    auto key = keys.get_or_create("analysis-evidence.db");
    ASSERT_TRUE(key);
    auto migrations = knowledge::kg_migrations();
    migrations.push_back({knowledge::kKgEvidenceSchemaVersion, knowledge::kg_evidence_migration_sql()});
    MigrationOutcome outcome;
    auto db = open_and_migrate_consistently(root->file("analysis-evidence.db"), *key, migrations, outcome,
                                             knowledge::register_kg_writer);
    ASSERT_TRUE(db) << db.error().message;
    ASSERT_FALSE(outcome.degraded) << outcome.failure;
    knowledge::KgStore store(std::move(*db));
    ASSERT_TRUE(store.db().exec(R"sql(
INSERT INTO entities(id,type,name,key) VALUES(1,'project','Project','project'),(2,'date','Date','date');
INSERT INTO knowledge_facts(id,subject,predicate,object) VALUES(1,1,'deadline',2),(2,1,'other',2);
INSERT INTO items(id,source,ext_id,hash) VALUES(1,'files','source','v1');
INSERT INTO chunks(id,item_id,ord,text) VALUES(1,1,0,'Český deadline'),(2,1,1,'unrelated');
INSERT INTO knowledge_revisions(id,item_id,sequence,source_hash,indexed_at,current) VALUES(1,1,1,'v1',1,1);
INSERT INTO knowledge_evidence(id,revision_id,chunk_id,excerpt,precision) VALUES(1,1,1,'Český deadline','passage');
INSERT INTO knowledge_support_sets(id,fact_id,state) VALUES(1,1,'current'),(2,2,'current');
INSERT INTO knowledge_support_evidence(support_id,evidence_id) VALUES(1,1);
)sql"));
    auto category = store.create_category("Dates");
    ASSERT_TRUE(category);
    auto generation = store.knowledge_generation();
    ASSERT_TRUE(generation);
    // "Český" occupies seven UTF-8 bytes; neither endpoint may split a character.
    EXPECT_FALSE(store.bind_analysis_evidence(1, {1, 1, 7}, *generation));
    EXPECT_FALSE(store.bind_analysis_evidence(1, {1, 0, 6}, *generation));
    EXPECT_FALSE(store.bind_analysis_evidence(1, {1, 0, 100}, *generation));
    EXPECT_FALSE(store.bind_analysis_evidence(1, {2, 0, 2}, *generation));
    EXPECT_FALSE(store.bind_analysis_evidence(2, {1, 0, 7}, *generation));
    EXPECT_FALSE(store.bind_analysis_evidence(1, {1, 0, 7}, *generation - 1));
    auto before = store.db().prepare("SELECT count(*) FROM knowledge_evidence");
    ASSERT_TRUE(before.step());
    EXPECT_EQ(before.col_int(0), 1);
    auto bound = store.bind_analysis_evidence(1, {1, 0, 7}, *generation);
    ASSERT_TRUE(bound) << bound.error().message;
    auto exact = store.db().prepare("SELECT e.excerpt,e.byte_start,e.byte_end,se.support_id FROM knowledge_evidence e "
                                    "JOIN knowledge_support_evidence se ON se.evidence_id=e.id WHERE e.id=?1");
    exact.bind(1, bound->evidence_id);
    ASSERT_TRUE(exact.step());
    EXPECT_EQ(exact.col_text(0), "Český");
    EXPECT_EQ(exact.col_int(1), 0);
    EXPECT_EQ(exact.col_int(2), 7);
    EXPECT_EQ(exact.col_int(3), 1);
    auto again = store.bind_analysis_evidence(1, {1, 0, 7}, bound->generation);
    ASSERT_TRUE(again) << again.error().message;
    EXPECT_EQ(again->evidence_id, bound->evidence_id);
    EXPECT_EQ(again->generation, bound->generation);
    ASSERT_TRUE(store.set_category_assignment(1, *category, "automatic", "Deadline evidence", bound->evidence_id,
                                               bound->generation));
    ASSERT_EQ(store.category_assignments(1).size(), 1u);
    ASSERT_TRUE(store.db().exec("UPDATE chunks SET text='changed deadline' WHERE id=1"));
    generation = store.knowledge_generation();
    ASSERT_TRUE(generation);
    EXPECT_FALSE(store.bind_analysis_evidence(1, {1, 0, 7}, *generation));
    EXPECT_FALSE(store.set_category_assignment(1, *category, "automatic", "Stale passage", bound->evidence_id, *generation));
    ASSERT_TRUE(store.db().exec("UPDATE knowledge_revisions SET current=0 WHERE id=1"));
    generation = store.knowledge_generation();
    ASSERT_TRUE(generation);
    EXPECT_FALSE(store.bind_analysis_evidence(1, {1, 0, 7}, *generation));
}

TEST(Database, SourceRevisionKeepsUnchangedEvidenceAndIndependentSupport) {
    TempRoot t;
    auto root = DataRoot::open(t.path);
    ASSERT_TRUE(root);
    KeyStore keys(root->keys());
    ASSERT_TRUE(keys.load());
    auto key = keys.get_or_create("revisions.db");
    ASSERT_TRUE(key);
    auto migrations = knowledge::kg_migrations();
    migrations.push_back({knowledge::kKgEvidenceSchemaVersion, knowledge::kg_evidence_migration_sql()});
    MigrationOutcome outcome;
    auto db = open_and_migrate_consistently(root->file("revisions.db"), *key, migrations, outcome, knowledge::register_kg_writer);
    ASSERT_TRUE(db) << db.error().message;
    ASSERT_FALSE(outcome.degraded) << outcome.failure;
    knowledge::KgStore store(std::move(*db));
    const auto put = [&](const std::string& ext, const std::string& hash, std::vector<knowledge::Chunk> chunks) {
        knowledge::ItemInput in;
        in.source = "files";
        in.ext_id = ext;
        in.title = ext;
        in.hash = hash;
        in.updated_at = 1000;
        in.chunks = std::move(chunks);
        auto id = store.put_item(in, 2000);
        EXPECT_TRUE(id) << (id ? "" : id.error().message);
        return id ? *id : 0;
    };
    const auto number = [&](const std::string& sql) -> long long {
        auto s = store.db().prepare(sql);
        return s.step() ? s.col_int(0) : -1;
    };
    const auto text = [&](const std::string& sql) {
        auto s = store.db().prepare(sql);
        return s.step() ? s.col_text(0) : std::string("<none>");
    };
    const std::string starts = "Project Atlas starts on 1 May.", budget = "Budget is approved.";
    const long long a = put("a", "h1", {{"", starts, 0}, {"", budget, 0}});
    const long long b = put("b", "h1", {{"", "Atlas kicks off on 1 May.", 0}});
    ASSERT_TRUE(a && b);
    EXPECT_EQ(number("SELECT count(*) FROM knowledge_revisions WHERE current=1 AND sequence=1"), 2);
    const long long a_starts = number("SELECT id FROM chunks WHERE item_id=" + std::to_string(a) + " AND ord=0");
    const long long a_budget = number("SELECT id FROM chunks WHERE item_id=" + std::to_string(a) + " AND ord=1");
    const long long b_starts = number("SELECT id FROM chunks WHERE item_id=" + std::to_string(b));
    const long long atlas = store.resolve_entity("project", "Atlas", "", 1), may = store.resolve_entity("date", "1 May", "", 1);
    ASSERT_TRUE(atlas && may);

    // Two Sources state one fact; a repeated finding from the same passage adds no second support.
    ASSERT_TRUE(store.add_relation(atlas, "starts", may, 0.9, "2026", "", a_starts));
    ASSERT_TRUE(store.add_relation(atlas, "starts", may, 0.9, "2026", "", a_starts));
    ASSERT_TRUE(store.add_relation(atlas, "starts", may, 0.9, "2026", "", b_starts));
    ASSERT_TRUE(store.add_relation(atlas, "budget", may, 0.9, "2026", "", a_budget));
    ASSERT_TRUE(store.add_relation(atlas, "reports_to", may, 1.0, "", "", 0));
    EXPECT_EQ(number("SELECT count(*) FROM knowledge_facts"), 2);
    EXPECT_EQ(number("SELECT count(*) FROM knowledge_support_sets WHERE state='unverified' AND legacy_relation_id IS NOT NULL"), 3);
    EXPECT_EQ(number("SELECT count(*) FROM knowledge_evidence WHERE precision='passage'"), 3);
    const long long starts_fact = number("SELECT id FROM knowledge_facts WHERE predicate='starts'");
    const long long budget_fact = number("SELECT id FROM knowledge_facts WHERE predicate='budget'");
    const std::string active = " AND state IN ('current','unverified')";

    // An exact excerpt and an automatic category on the passage that will change; an explicit support, an exact
    // excerpt and an automatic category on the passage that will only move.
    auto generation = store.knowledge_generation();
    ASSERT_TRUE(generation);
    auto changing = store.bind_analysis_evidence(starts_fact, {a_starts, 0, 13}, *generation);
    ASSERT_TRUE(changing) << changing.error().message;
    auto projects = store.create_category("Projects");
    ASSERT_TRUE(projects);
    ASSERT_TRUE(store.set_category_assignment(starts_fact, *projects, "automatic", "names a project", changing->evidence_id,
                                               store.knowledge_generation()));
    ASSERT_TRUE(store.db().exec(
        "INSERT INTO knowledge_evidence(id,revision_id,chunk_id,excerpt,precision,byte_start,byte_end) "
        "SELECT 900,v.id," + std::to_string(a_budget) + ",'Budget','exact',0,6 FROM knowledge_revisions v WHERE v.item_id=" +
        std::to_string(a) + ";"
        "INSERT INTO knowledge_support_sets(id,fact_id,state) VALUES(900," + std::to_string(budget_fact) + ",'current');"
        "INSERT INTO knowledge_support_evidence(support_id,evidence_id) VALUES(900,900);"));
    ASSERT_TRUE(store.set_category_assignment(budget_fact, *projects, "automatic", "a project budget", 900, store.knowledge_generation()));
    ASSERT_TRUE(store.add_fact(atlas, "Atlas is the internal name.", "", 1, "files", "a"));

    // Checking the same content again creates no revision and changes no support.
    generation = store.knowledge_generation();
    ASSERT_EQ(put("a", "h1", {{"", starts, 0}, {"", budget, 0}}), a);
    EXPECT_EQ(number("SELECT count(*) FROM knowledge_revisions WHERE item_id=" + std::to_string(a)), 1);
    EXPECT_EQ(number("SELECT count(*) FROM knowledge_support_sets WHERE 1=1" + active), 4);

    // The revision moves the budget passage unchanged and replaces the passage that stated the start.
    ASSERT_EQ(put("a", "h2", {{"", budget, 0}, {"", "Project Atlas starts on 1 June.", 0}}), a);
    EXPECT_GT(*store.knowledge_generation(), *generation);
    EXPECT_EQ(number("SELECT count(*) FROM knowledge_revisions WHERE item_id=" + std::to_string(a)), 2);
    EXPECT_EQ(text("SELECT source_hash||':'||sequence FROM knowledge_revisions WHERE current=1 AND item_id=" + std::to_string(a)), "h2:2");
    const std::string current_a = "(SELECT id FROM knowledge_revisions WHERE current=1 AND item_id=" + std::to_string(a) + ")";
    // The explicit support stays current on the new revision, with its excerpt and its automatic category.
    EXPECT_EQ(text("SELECT state FROM knowledge_support_sets WHERE id=900"), "current");
    EXPECT_EQ(number("SELECT count(*) FROM knowledge_support_evidence se JOIN knowledge_evidence e ON e.id=se.evidence_id "
                     "WHERE se.support_id=900 AND e.excerpt='Budget' AND e.chunk_id=" + std::to_string(a_budget) +
                     " AND e.revision_id=" + current_a),
              1);
    auto kept = store.category_assignments(budget_fact);
    ASSERT_EQ(kept.size(), 1u);
    EXPECT_EQ(kept[0].decision, "automatic");
    EXPECT_TRUE(store.set_category_assignment(budget_fact, *projects, "automatic", "still a project budget",
                                              number("SELECT evidence_id FROM knowledge_support_evidence WHERE support_id=900"),
                                              store.knowledge_generation()));
    // The previous revision keeps its own excerpt as history.
    EXPECT_EQ(number("SELECT count(*) FROM knowledge_evidence WHERE id=900 AND revision_id<>" + current_a), 1);
    // Extraction cannot tell which passages a finding relied on, so the moved passage's finding awaits a recheck.
    EXPECT_EQ(number("SELECT count(*) FROM knowledge_support_sets WHERE legacy_relation_id IS NULL AND state='unsupported' AND fact_id=" +
                     std::to_string(budget_fact)),
              1);
    EXPECT_EQ(number("SELECT count(*) FROM extract_queue WHERE chunk_id=" + std::to_string(a_budget)), 1);
    // The replaced passage no longer supports the start; the other Source still does.
    EXPECT_EQ(number("SELECT count(*) FROM knowledge_support_sets WHERE fact_id=" + std::to_string(starts_fact) + active), 1);
    EXPECT_EQ(number("SELECT count(*) FROM knowledge_support_sets s JOIN knowledge_support_evidence se ON se.support_id=s.id "
                     "JOIN knowledge_evidence e ON e.id=se.evidence_id WHERE s.fact_id=" + std::to_string(starts_fact) + active +
                     " AND e.chunk_id=" + std::to_string(b_starts)),
              1);
    EXPECT_TRUE(store.category_assignments(starts_fact).empty());
    generation = store.knowledge_generation();
    EXPECT_FALSE(store.bind_analysis_evidence(starts_fact, {a_starts, 0, 13}, *generation));
    // The correction follows the item to its current revision.
    EXPECT_EQ(number("SELECT count(*) FROM knowledge_annotations WHERE revision_id=" + current_a), 1);

    // Extraction finds the budget again in the moved passage and the start in the new one.
    const long long supports = number("SELECT count(*) FROM knowledge_support_sets");
    ASSERT_TRUE(store.add_relation(atlas, "budget", may, 0.9, "2026", "", a_budget));
    EXPECT_EQ(number("SELECT count(*) FROM knowledge_support_sets"), supports);
    EXPECT_EQ(number("SELECT count(*) FROM knowledge_support_sets WHERE fact_id=" + std::to_string(budget_fact) + active), 2);
    const long long a_new = number("SELECT id FROM chunks WHERE item_id=" + std::to_string(a) + " AND ord=1");
    ASSERT_NE(a_new, a_starts);
    ASSERT_TRUE(store.add_relation(atlas, "starts", may, 0.9, "2026", "", a_new));
    EXPECT_EQ(number("SELECT count(*) FROM knowledge_support_sets WHERE fact_id=" + std::to_string(starts_fact) + active), 2);

    // Forgetting the other Source leaves the fact with the support that remains; forgetting both removes it.
    ASSERT_TRUE(store.db().exec("DELETE FROM items WHERE id=" + std::to_string(b)));
    EXPECT_EQ(number("SELECT count(*) FROM knowledge_support_sets WHERE fact_id=" + std::to_string(starts_fact) + active), 1);
    ASSERT_TRUE(store.db().exec("DELETE FROM items WHERE id=" + std::to_string(a)));
    EXPECT_EQ(number("SELECT count(*) FROM knowledge_facts"), 0);
    EXPECT_EQ(number("SELECT (SELECT count(*) FROM knowledge_revisions)+(SELECT count(*) FROM knowledge_evidence)+"
                     "(SELECT count(*) FROM knowledge_support_sets)+(SELECT count(*) FROM knowledge_annotations)"),
              0);
}

TEST(Database, SourceRevisionRechecksOnlyFindingsWhoseExtractionInputsChanged) {
    TempRoot t;
    auto root = DataRoot::open(t.path);
    ASSERT_TRUE(root);
    KeyStore keys(root->keys());
    ASSERT_TRUE(keys.load());
    auto key = keys.get_or_create("inputs.db");
    ASSERT_TRUE(key);
    auto migrations = knowledge::kg_migrations();
    migrations.push_back({knowledge::kKgEvidenceSchemaVersion, knowledge::kg_evidence_migration_sql()});
    MigrationOutcome outcome;
    auto db = open_and_migrate_consistently(root->file("inputs.db"), *key, migrations, outcome, knowledge::register_kg_writer);
    ASSERT_TRUE(db) << db.error().message;
    ASSERT_FALSE(outcome.degraded) << outcome.failure;
    knowledge::KgStore store(std::move(*db));
    const auto put = [&](const std::string& ext, const std::string& hash, long long updated, std::vector<knowledge::Chunk> chunks) {
        knowledge::ItemInput in;
        in.source = "files";
        in.ext_id = ext;
        in.title = ext;
        in.hash = hash;
        in.updated_at = updated;
        in.chunks = std::move(chunks);
        auto id = store.put_item(in, 2000);
        EXPECT_TRUE(id) << (id ? "" : id.error().message);
        return id ? *id : 0;
    };
    const auto number = [&](const std::string& sql) -> long long {
        auto s = store.db().prepare(sql);
        return s.step() ? s.col_int(0) : -1;
    };
    const auto chunk = [&](long long item, int ord) {
        return number("SELECT id FROM chunks WHERE item_id=" + std::to_string(item) + " AND ord=" + std::to_string(ord));
    };
    const auto findings = [&](long long id) { return number("SELECT count(*) FROM relations WHERE chunk_id=" + std::to_string(id)); };
    const auto queued = [&](long long id) { return number("SELECT count(*) FROM extract_queue WHERE chunk_id=" + std::to_string(id)); };
    // Unverified support whose passage evidence belongs to the item's current revision.
    const auto supported = [&](long long id) {
        return number("SELECT count(*) FROM knowledge_support_sets s JOIN knowledge_support_evidence se ON se.support_id=s.id "
                      "JOIN knowledge_evidence e ON e.id=se.evidence_id JOIN knowledge_revisions v ON v.id=e.revision_id "
                      "WHERE s.state='unverified' AND v.current=1 AND e.chunk_id=" + std::to_string(id));
    };
    const std::string intro = "Atlas is a logistics project.", plan = "Atlas starts on 1 May.", budget = "Atlas has an approved budget.";
    const long long doc = put("doc", "h1", 1000, {{"", intro, 0}, {"", plan, 0}, {"", budget, 0}});
    const long long other = put("other", "h1", 1000, {{"", "Atlas reports to operations.", 0}});
    ASSERT_TRUE(doc && other);
    const long long c_intro = chunk(doc, 0), c_plan = chunk(doc, 1), c_budget = chunk(doc, 2), c_note = chunk(other, 0);
    const long long atlas = store.resolve_entity("project", "Atlas", "", 1), value = store.resolve_entity("topic", "value", "", 1);
    ASSERT_TRUE(atlas && value);
    const auto find = [&](const std::string& predicate, long long id) {
        return store.add_relation(atlas, predicate, value, 0.9, "2026", "", id) != 0;
    };
    // Two extraction requests: intro with plan, budget with the other Source's passage. The budget finding took its
    // date from the item.
    ASSERT_TRUE(find("is", c_intro) && find("starts", c_plan) && find("budget", c_budget) && find("reports", c_note));
    ASSERT_TRUE(store.record_extraction({c_intro, c_plan}, {}));
    ASSERT_TRUE(store.record_extraction({c_budget, c_note}, {c_budget}));
    store.clear_extract_queue();

    // Replacing the plan rechecks the passage extracted beside it and nothing else.
    ASSERT_EQ(put("doc", "h2", 1000, {{"", intro, 0}, {"", "Atlas starts on 1 June.", 0}, {"", budget, 0}}), doc);
    const long long c_plan2 = chunk(doc, 1);
    ASSERT_NE(c_plan2, c_plan);
    EXPECT_EQ(findings(c_intro), 0);
    EXPECT_EQ(queued(c_intro), 1);
    EXPECT_EQ(supported(c_intro), 0);
    EXPECT_EQ(findings(c_budget), 1);
    EXPECT_EQ(queued(c_budget), 0);
    EXPECT_EQ(supported(c_budget), 1);
    EXPECT_EQ(findings(c_note), 1);
    EXPECT_EQ(queued(c_note), 0);
    EXPECT_EQ(queued(c_plan2), 1);

    // The model answers for intro and the new plan; the same finding revives its support.
    ASSERT_TRUE(find("is", c_intro) && find("starts", c_plan2));
    ASSERT_TRUE(store.record_extraction({c_intro, c_plan2}, {}));
    store.clear_extract_queue();
    EXPECT_EQ(supported(c_intro), 1);

    // Only the item's date changes: the finding dated from the item is rechecked, the others are kept.
    ASSERT_EQ(put("doc", "h2", 5000, {{"", intro, 0}, {"", "Atlas starts on 1 June.", 0}, {"", budget, 0}}), doc);
    EXPECT_EQ(number("SELECT count(*) FROM knowledge_revisions WHERE item_id=" + std::to_string(doc)), 2);
    EXPECT_EQ(findings(c_budget), 0);
    EXPECT_EQ(queued(c_budget), 1);
    EXPECT_EQ(findings(c_intro), 1);
    EXPECT_EQ(findings(c_plan2), 1);
    EXPECT_EQ(queued(c_intro) + queued(c_plan2), 0);
    EXPECT_EQ(findings(c_note), 1);
    ASSERT_TRUE(find("budget", c_budget));
    ASSERT_TRUE(store.record_extraction({c_budget}, {c_budget}));
    store.clear_extract_queue();

    // A passage whose own date changes is rechecked together with the passage extracted beside it.
    ASSERT_EQ(put("doc", "h3", 5000, {{"", intro, 0}, {"", "Atlas starts on 1 June.", 777}, {"", budget, 0}}), doc);
    EXPECT_EQ(chunk(doc, 1), c_plan2);
    EXPECT_EQ(findings(c_plan2) + findings(c_intro), 0);
    EXPECT_EQ(queued(c_plan2) + queued(c_intro), 2);
    EXPECT_EQ(findings(c_budget), 1);
    EXPECT_EQ(queued(c_budget), 0);

    // The other Source's finding was produced beside the budget passage, so forgetting the document rechecks it.
    EXPECT_EQ(findings(c_note), 1);
    ASSERT_TRUE(store.remove_item("files", "doc", true));
    EXPECT_EQ(findings(c_note), 0);
    EXPECT_EQ(queued(c_note), 1);
    EXPECT_EQ(number("SELECT count(*) FROM knowledge_extraction_inputs"), 0);
    EXPECT_EQ(number("SELECT count(*) FROM knowledge_support_sets WHERE state IN ('current','unverified')"), 0);
}

TEST(Database, SourceRevisionInvalidatesDerivationsThroughRequiredFacts) {
    TempRoot t;
    auto root = DataRoot::open(t.path);
    ASSERT_TRUE(root);
    KeyStore keys(root->keys());
    ASSERT_TRUE(keys.load());
    auto key = keys.get_or_create("derived.db");
    ASSERT_TRUE(key);
    auto migrations = knowledge::kg_migrations();
    migrations.push_back({knowledge::kKgEvidenceSchemaVersion, knowledge::kg_evidence_migration_sql()});
    MigrationOutcome outcome;
    auto db = open_and_migrate_consistently(root->file("derived.db"), *key, migrations, outcome, knowledge::register_kg_writer);
    ASSERT_TRUE(db) << db.error().message;
    ASSERT_FALSE(outcome.degraded) << outcome.failure;
    knowledge::KgStore store(std::move(*db));
    const auto put = [&](const std::string& ext, const std::string& hash, const std::string& passage) {
        knowledge::ItemInput in;
        in.source = "files";
        in.ext_id = ext;
        in.title = ext;
        in.hash = hash;
        in.updated_at = 1000;
        in.chunks = {{"", passage, 0}};
        auto id = store.put_item(in, 2000);
        EXPECT_TRUE(id) << (id ? "" : id.error().message);
        return id ? *id : 0;
    };
    const auto number = [&](const std::string& sql) -> long long {
        auto s = store.db().prepare(sql);
        return s.step() ? s.col_int(0) : -1;
    };
    const auto text = [&](const std::string& sql) {
        auto s = store.db().prepare(sql);
        return s.step() ? s.col_text(0) : std::string("<none>");
    };
    const long long a = put("a", "h1", "Atlas starts on 1 May."), b = put("b", "h1", "The Atlas team has eight people.");
    ASSERT_TRUE(a && b);
    const long long atlas = store.resolve_entity("project", "Atlas", "", 1), value = store.resolve_entity("topic", "value", "", 1);
    ASSERT_TRUE(atlas && value);
    ASSERT_TRUE(store.add_relation(atlas, "starts", value, 0.9, "2026", "", number("SELECT id FROM chunks WHERE item_id=" + std::to_string(a))));
    ASSERT_TRUE(store.add_relation(atlas, "team", value, 0.9, "2026", "", number("SELECT id FROM chunks WHERE item_id=" + std::to_string(b))));
    const std::string starts = std::to_string(number("SELECT id FROM knowledge_facts WHERE predicate='starts'"));
    const std::string team = std::to_string(number("SELECT id FROM knowledge_facts WHERE predicate='team'"));
    // 701 needs both Sources' facts, 702 needs 701, and 703 is derived independently from either fact.
    ASSERT_TRUE(store.db().exec(
        "INSERT INTO knowledge_facts(id,subject,predicate,object) VALUES(701," + std::to_string(atlas) + ",'staffed_at_start'," +
        std::to_string(value) + "),(702," + std::to_string(atlas) + ",'ready'," + std::to_string(value) + "),(703," +
        std::to_string(atlas) + ",'active'," + std::to_string(value) + ");"
        "INSERT INTO knowledge_support_sets(id,fact_id,state) VALUES(701,701,'current'),(702,702,'current'),(703,703,'current'),(704,703,'current');"
        "INSERT INTO knowledge_support_facts(support_id,required_fact_id) VALUES(701," + starts + "),(701," + team + "),(702,701),(703," +
        starts + "),(704," + team + ");"));
    const auto state = [&](int support) { return text("SELECT state FROM knowledge_support_sets WHERE id=" + std::to_string(support)); };
    store.clear_extract_queue();

    // The revision replaces the only passage stating the start while no analysis has run on the new text.
    ASSERT_EQ(put("a", "h2", "Atlas starts on 1 June."), a);
    EXPECT_EQ(number("SELECT count(*) FROM knowledge_support_sets WHERE state IN ('current','unverified') AND fact_id=" + starts), 0);
    EXPECT_EQ(number("SELECT count(*) FROM extract_queue"), 1);
    // The replaced passage's support is history of the old revision, not a finding waiting to be confirmed again.
    EXPECT_EQ(number("SELECT count(*) FROM knowledge_support_sets WHERE state='superseded' AND fact_id=" + starts), 1);
    EXPECT_EQ(state(701), "unsupported");
    EXPECT_EQ(state(702), "unsupported");
    EXPECT_EQ(state(703), "unsupported");
    EXPECT_EQ(state(704), "current");
    EXPECT_EQ(number("SELECT count(*) FROM knowledge_support_sets WHERE state IN ('current','unverified') AND fact_id=" + team), 1);

    // Extraction later states the start again from the new passage. The derivations were made from the old support
    // and stay out of current knowledge until they are derived again.
    ASSERT_TRUE(store.add_relation(atlas, "starts", value, 0.9, "2026", "", number("SELECT id FROM chunks WHERE item_id=" + std::to_string(a))));
    EXPECT_EQ(number("SELECT count(*) FROM knowledge_support_sets WHERE state='unverified' AND fact_id=" + starts), 1);
    EXPECT_EQ(state(701), "unsupported");
    EXPECT_EQ(state(702), "unsupported");
    EXPECT_EQ(state(703), "unsupported");
    EXPECT_EQ(state(704), "current");
}

TEST(Database, DerivationDepthAndRevisionHistoryAreBounded) {
    TempRoot t;
    auto root = DataRoot::open(t.path);
    ASSERT_TRUE(root);
    KeyStore keys(root->keys());
    ASSERT_TRUE(keys.load());
    auto key = keys.get_or_create("bounds.db");
    ASSERT_TRUE(key);
    auto migrations = knowledge::kg_migrations();
    migrations.push_back({knowledge::kKgEvidenceSchemaVersion, knowledge::kg_evidence_migration_sql()});
    MigrationOutcome outcome;
    auto db = open_and_migrate_consistently(root->file("bounds.db"), *key, migrations, outcome, knowledge::register_kg_writer);
    ASSERT_TRUE(db) << db.error().message;
    ASSERT_FALSE(outcome.degraded) << outcome.failure;
    knowledge::KgStore store(std::move(*db));
    const auto put = [&](const std::string& ext, int version) {
        knowledge::ItemInput in;
        in.source = "files";
        in.ext_id = ext;
        in.title = ext;
        in.hash = "h" + std::to_string(version);
        in.updated_at = 1000;
        in.chunks = {{"", "Atlas starts in month " + std::to_string(version) + ".", 0}};
        auto id = store.put_item(in, 2000);
        EXPECT_TRUE(id) << (id ? "" : id.error().message);
        return id ? *id : 0;
    };
    const auto number = [&](const std::string& sql) -> long long {
        auto s = store.db().prepare(sql);
        return s.step() ? s.col_int(0) : -1;
    };
    const long long a = put("a", 1);
    ASSERT_TRUE(a);
    const long long atlas = store.resolve_entity("project", "Atlas", "", 1), value = store.resolve_entity("topic", "value", "", 1);
    ASSERT_TRUE(atlas && value);
    const auto state_fact = [&](const std::string& predicate) {
        return store.add_relation(atlas, predicate, value, 0.9, "2026", "", number("SELECT id FROM chunks WHERE item_id=" + std::to_string(a))) != 0;
    };
    ASSERT_TRUE(state_fact("starts"));
    const long long base = number("SELECT id FROM knowledge_facts WHERE predicate='starts'");
    const auto derive = [&](long long id, long long required) {
        return store.db().exec("INSERT INTO knowledge_facts(id,subject,predicate,object) VALUES(" + std::to_string(id) + "," +
                               std::to_string(atlas) + ",'derived-" + std::to_string(id) + "'," + std::to_string(value) + ");"
                               "INSERT INTO knowledge_support_sets(id,fact_id,state) VALUES(" + std::to_string(id) + "," +
                               std::to_string(id) + ",'current');"
                               "INSERT INTO knowledge_support_facts(support_id,required_fact_id) VALUES(" + std::to_string(id) + "," +
                               std::to_string(required) + ")");
    };
    // The extracted fact and 31 derivations form the longest permitted chain.
    for (int level = 1; level < knowledge::kMaxDerivationDepth; ++level)
        ASSERT_TRUE(derive(1000 + level, level == 1 ? base : 1000 + level - 1)) << level;
    ASSERT_TRUE(store.db().exec("SAVEPOINT deep"));
    EXPECT_FALSE(derive(1032, 1031));
    ASSERT_TRUE(store.db().exec("ROLLBACK TO deep; RELEASE deep"));
    // Joining two short chains may not exceed the bound either.
    ASSERT_TRUE(derive(2001, base));
    ASSERT_TRUE(derive(2002, 2001));
    EXPECT_FALSE(store.db().exec("INSERT INTO knowledge_support_facts(support_id,required_fact_id) VALUES(2001,1031)"));
    EXPECT_TRUE(store.db().exec("INSERT INTO knowledge_support_facts(support_id,required_fact_id) VALUES(2001,1029)"));
    EXPECT_EQ(number("SELECT count(*) FROM knowledge_support_sets WHERE state='current'"), 33);

    // A revision replacing the only passage invalidates the whole chain in one write.
    ASSERT_EQ(put("a", 2), a);
    EXPECT_EQ(number("SELECT count(*) FROM knowledge_support_sets WHERE state='unsupported'"), 33);
    EXPECT_EQ(number("SELECT count(*) FROM knowledge_support_sets WHERE state='superseded'"), 1);

    // History keeps four superseded revisions; older excerpts, supports and facts they alone supported are removed.
    for (int version = 2; version <= 7; ++version) {
        if (version > 2) ASSERT_EQ(put("a", version), a);
        ASSERT_TRUE(state_fact("starts-" + std::to_string(version)));
    }
    EXPECT_EQ(number("SELECT count(*) FROM knowledge_revisions WHERE item_id=" + std::to_string(a)), 5);
    EXPECT_EQ(number("SELECT min(sequence) FROM knowledge_revisions WHERE item_id=" + std::to_string(a)), 3);
    EXPECT_EQ(number("SELECT count(*) FROM knowledge_evidence"), 5);
    EXPECT_EQ(number("SELECT count(*) FROM knowledge_facts WHERE predicate IN ('starts','starts-2')"), 0);
    EXPECT_EQ(number("SELECT count(*) FROM knowledge_facts WHERE predicate LIKE 'starts-%'"), 5);
    // The derivations required the pruned fact and went with it.
    EXPECT_EQ(number("SELECT count(*) FROM knowledge_facts WHERE predicate LIKE 'derived-%'"), 0);

    // Forgetting cascades through a full-depth chain without reaching the trigger depth limit.
    const long long current = number("SELECT id FROM knowledge_facts WHERE predicate='starts-7'");
    for (int level = 1; level < knowledge::kMaxDerivationDepth; ++level)
        ASSERT_TRUE(derive(3000 + level, level == 1 ? current : 3000 + level - 1)) << level;
    ASSERT_TRUE(store.remove_item("files", "a", true));
    EXPECT_EQ(number("SELECT (SELECT count(*) FROM knowledge_facts)+(SELECT count(*) FROM knowledge_support_sets)+"
                     "(SELECT count(*) FROM knowledge_revisions)+(SELECT count(*) FROM knowledge_evidence)"),
              0);
}

TEST(Database, LargeSourceChangeWithdrawsInStepsAndRecoversIfInterrupted) {
    TempRoot t;
    auto root = DataRoot::open(t.path);
    ASSERT_TRUE(root);
    KeyStore keys(root->keys());
    ASSERT_TRUE(keys.load());
    auto key = keys.get_or_create("steps.db");
    ASSERT_TRUE(key);
    auto migrations = knowledge::kg_migrations();
    migrations.push_back({knowledge::kKgEvidenceSchemaVersion, knowledge::kg_evidence_migration_sql()});
    MigrationOutcome outcome;
    auto db = open_and_migrate_consistently(root->file("steps.db"), *key, migrations, outcome, knowledge::register_kg_writer);
    ASSERT_TRUE(db) << db.error().message;
    ASSERT_FALSE(outcome.degraded) << outcome.failure;
    knowledge::KgStore store(std::move(*db));
    const auto number = [&](const std::string& sql) -> long long {
        auto s = store.db().prepare(sql);
        return s.step() ? s.col_int(0) : -1;
    };
    constexpr int kPassages = 2100;  // above the 2,048-passage threshold for stepping
    knowledge::ItemInput in;
    in.source = "files";
    in.ext_id = "large";
    in.title = "large";
    in.hash = "h1";
    in.updated_at = 1000;
    for (int n = 0; n < kPassages; ++n) in.chunks.push_back({"", "Passage " + std::to_string(n) + ".", 0});
    const auto item = store.put_item(in, 2000);
    ASSERT_TRUE(item) << item.error().message;
    const long long one = store.resolve_entity("project", "Atlas", "", 1), two = store.resolve_entity("topic", "value", "", 1);
    const auto extract_all = [&] {
        int n = 0;
        for (const auto& c : store.chunks_of_item(*item))
            if (!number("SELECT count(*) FROM relations WHERE chunk_id=" + std::to_string(c.id)))
                ASSERT_TRUE(store.add_relation(one, "fact-" + std::to_string(c.id), two, 0.9, "2026", "", c.id)) << ++n;
        store.clear_extract_queue();
    };
    extract_all();
    const std::string active = "SELECT count(*) FROM knowledge_support_sets WHERE state IN ('current','unverified')";
    ASSERT_EQ(number(active), kPassages);
    // An exact excerpt on a passage that stays: a recheck of its legacy finding does not touch it.
    const long long kept = number("SELECT id FROM chunks WHERE item_id=" + std::to_string(*item) + " AND ord=1");
    const long long kept_fact = number("SELECT fact_id FROM knowledge_support_sets s JOIN relations r ON r.id=s.legacy_relation_id "
                                       "WHERE r.chunk_id=" + std::to_string(kept));
    ASSERT_TRUE(store.db().exec(
        "INSERT INTO knowledge_evidence(id,revision_id,chunk_id,excerpt,precision,byte_start,byte_end) "
        "SELECT 90000,id," + std::to_string(kept) + ",'Passage','exact',0,7 FROM knowledge_revisions WHERE current=1 AND item_id=" +
        std::to_string(*item) + ";"
        "INSERT INTO knowledge_support_sets(id,fact_id,state) VALUES(90000," + std::to_string(kept_fact) + ",'current');"
        "INSERT INTO knowledge_support_evidence(support_id,evidence_id) VALUES(90000,90000);"));
    ASSERT_EQ(number(active), kPassages + 1);

    // The revision replaces every even passage. Stopping after the withdrawal step leaves the item as it was, its
    // findings awaiting recheck and its passages queued, never a partly replaced item.
    knowledge::ItemInput revised = in;
    revised.hash = "h2";
    for (size_t n = 0; n < revised.chunks.size(); n += 2) revised.chunks[n].text += " Revised.";
    ASSERT_TRUE(store.prepare_item_change(revised));
    EXPECT_EQ(store.item_hash("files", "large"), "h1");
    EXPECT_EQ(number("SELECT count(*) FROM knowledge_revisions WHERE item_id=" + std::to_string(*item)), 1);
    EXPECT_EQ(number("SELECT count(*) FROM chunks WHERE item_id=" + std::to_string(*item)), kPassages);
    EXPECT_EQ(number("SELECT count(*) FROM relations"), 0);
    EXPECT_EQ(number("SELECT count(*) FROM knowledge_support_sets WHERE state='unsupported'"), kPassages);
    EXPECT_EQ(number(active), 1);
    EXPECT_EQ(store.extract_pending(), kPassages);
    // Extraction of the unchanged item restores every finding.
    extract_all();
    EXPECT_EQ(number(active), kPassages + 1);
    EXPECT_EQ(number("SELECT count(*) FROM knowledge_support_sets"), kPassages + 1);

    // The complete revision: replaced passages' findings are history, the rest await recheck, the excerpt stays.
    ASSERT_TRUE(store.put_item(revised, 3000));
    EXPECT_EQ(store.item_hash("files", "large"), "h2");
    EXPECT_EQ(number("SELECT count(*) FROM knowledge_support_sets WHERE state='superseded'"), kPassages / 2);
    EXPECT_EQ(number("SELECT count(*) FROM knowledge_support_sets WHERE state='unsupported'"), kPassages / 2);
    EXPECT_EQ(number(active), 1);
    EXPECT_EQ(store.extract_pending(), kPassages);
    extract_all();
    EXPECT_EQ(number(active), kPassages + 1);

    // A large forget also proceeds in steps and leaves nothing.
    ASSERT_TRUE(store.remove_item("files", "large", true));
    EXPECT_EQ(number("SELECT (SELECT count(*) FROM knowledge_facts)+(SELECT count(*) FROM knowledge_support_sets)+"
                     "(SELECT count(*) FROM knowledge_evidence)+(SELECT count(*) FROM knowledge_revisions)+"
                     "(SELECT count(*) FROM chunks)+(SELECT count(*) FROM extract_queue)"),
              0);
}

TEST(Database, LargeSourceChangeIsStagedHiddenAndSwitched) {
    TempRoot t;
    auto root = DataRoot::open(t.path);
    ASSERT_TRUE(root);
    KeyStore keys(root->keys());
    ASSERT_TRUE(keys.load());
    auto key = keys.get_or_create("staged.db");
    ASSERT_TRUE(key);
    auto migrations = knowledge::kg_migrations();
    migrations.push_back({5, knowledge::kg_evidence_migration_sql()});
    MigrationOutcome outcome;
    auto db = open_and_migrate_consistently(root->file("staged.db"), *key, migrations, outcome, knowledge::register_kg_writer);
    ASSERT_TRUE(db) << db.error().message;
    ASSERT_FALSE(outcome.degraded) << outcome.failure;
    knowledge::KgStore store(std::move(*db));
    const auto number = [&](const std::string& sql) -> long long {
        auto s = store.db().prepare(sql);
        return s.step() ? s.col_int(0) : -1;
    };
    const std::string hidden = "SELECT count(*) FROM chunks c JOIN items i ON i.id=c.item_id WHERE i.source IN ('~staged','~retired')";
    constexpr int kPassages = 2100;  // a revision replacing half of them changes 2,100 passages, above the threshold
    knowledge::ItemInput in;
    in.source = "files";
    in.ext_id = "large";
    in.title = "large";
    in.hash = "h1";
    in.updated_at = 1000;
    for (int n = 0; n < kPassages; ++n) in.chunks.push_back({"", (n % 2 ? "Kept passage " : "Draft passage ") + std::to_string(n) + ".", 0});
    // A new large item is staged too; once written nothing hidden is left.
    const auto item = store.put_item(in, 2000);
    ASSERT_TRUE(item) << item.error().message;
    ASSERT_TRUE(store.finish_item_changes());
    EXPECT_EQ(number("SELECT count(*) FROM chunks WHERE item_id=" + std::to_string(*item)), kPassages);
    EXPECT_EQ(number(hidden), 0);
    EXPECT_EQ(number("SELECT count(*) FROM embed_queue"), kPassages);

    // Staged, not yet switched: the item, its search results and its counts are those of the first version.
    knowledge::ItemInput revised = in;
    revised.hash = "h2";
    for (int n = 0; n < kPassages; n += 2) revised.chunks[static_cast<size_t>(n)].text = "Final passage " + std::to_string(n) + ".";
    ASSERT_TRUE(store.prepare_item_change(revised));
    EXPECT_EQ(store.item_hash("files", "large"), "h1");
    EXPECT_EQ(number(hidden), kPassages / 2);
    EXPECT_TRUE(store.fts("Final", 10).empty());
    EXPECT_EQ(store.fts("Draft", 10).size(), 10u);
    EXPECT_EQ(store.counts("files").items, 1);
    EXPECT_EQ(store.counts("files").chunks, kPassages);
    EXPECT_EQ(store.find_items("", 10).size(), 1u);  // every item, and the stage is not one
    EXPECT_EQ(number("SELECT count(*) FROM embed_queue q JOIN chunks c ON c.id=q.chunk_id JOIN items i ON i.id=c.item_id "
                     "WHERE i.source='~staged'"),
              0);
    // Staging again resumes rather than duplicating.
    ASSERT_TRUE(store.prepare_item_change(revised));
    EXPECT_EQ(number(hidden), kPassages / 2);
    const long long staged_min = number("SELECT min(c.id) FROM chunks c JOIN items i ON i.id=c.item_id WHERE i.source='~staged'");

    // The switch adopts the staged passages and retires the replaced ones out of every reader's sight.
    ASSERT_TRUE(store.put_prepared_item(revised, 3000));
    EXPECT_EQ(store.item_hash("files", "large"), "h2");
    std::vector<std::string> texts;
    for (const auto& c : store.chunks_of_item(*item)) texts.push_back(c.text);
    ASSERT_EQ(texts.size(), static_cast<size_t>(kPassages));
    for (int n = 0; n < kPassages; ++n) EXPECT_EQ(texts[static_cast<size_t>(n)], revised.chunks[static_cast<size_t>(n)].text) << n;
    EXPECT_EQ(number("SELECT min(id) FROM chunks WHERE item_id=" + std::to_string(*item) + " AND text LIKE 'Final%'"), staged_min);
    EXPECT_EQ(number("SELECT count(*) FROM items WHERE source='~staged'"), 0);
    EXPECT_EQ(number(hidden), kPassages / 2);
    EXPECT_TRUE(store.fts("Draft", 10).empty());
    EXPECT_EQ(store.fts("Final", 10).size(), 10u);
    EXPECT_EQ(store.counts("files").chunks, kPassages);
    EXPECT_EQ(number("SELECT count(*) FROM chunks c JOIN items i ON i.id=c.item_id WHERE i.source='~retired' AND "
                     "(c.id IN (SELECT chunk_id FROM embed_queue) OR c.id IN (SELECT chunk_id FROM extract_queue))"),
              0);
    EXPECT_EQ(number("SELECT count(*) FROM chunks c JOIN embed_queue q ON q.chunk_id=c.id WHERE c.item_id=" + std::to_string(*item) +
                     " AND c.text LIKE 'Final%'"),
              kPassages / 2);
    ASSERT_TRUE(store.finish_item_changes());
    EXPECT_EQ(number(hidden), 0);
    EXPECT_EQ(number("SELECT count(*) FROM items"), 1);

    // A stage whose change is not written is discarded; a stage of another version is not used for a different one.
    knowledge::ItemInput third = revised, fourth = revised;
    third.hash = "h3";
    fourth.hash = "h4";
    for (int n = 1; n < kPassages; n += 2) {
        third.chunks[static_cast<size_t>(n)].text = "Third passage " + std::to_string(n) + ".";
        fourth.chunks[static_cast<size_t>(n)].text = "Fourth passage " + std::to_string(n) + ".";
    }
    ASSERT_TRUE(store.prepare_item_change(third));
    EXPECT_EQ(number(hidden), kPassages / 2);
    ASSERT_TRUE(store.discard_item_change("files", "large"));
    EXPECT_EQ(number(hidden), 0);
    ASSERT_TRUE(store.prepare_item_change(third));
    ASSERT_TRUE(store.put_prepared_item(fourth, 4000));
    ASSERT_TRUE(store.finish_item_changes());
    texts.clear();
    for (const auto& c : store.chunks_of_item(*item)) texts.push_back(c.text);
    ASSERT_EQ(texts.size(), static_cast<size_t>(kPassages));
    EXPECT_EQ(texts[1], "Fourth passage 1.");
    EXPECT_EQ(number(hidden), 0);
    EXPECT_TRUE(store.fts("Third", 10).empty());

    // Forgetting an item while a change of it is staged leaves no copy of its text.
    ASSERT_TRUE(store.prepare_item_change(third));
    EXPECT_GT(number(hidden), 0);
    ASSERT_TRUE(store.remove_item("files", "large", true));
    EXPECT_EQ(number("SELECT (SELECT count(*) FROM chunks)+(SELECT count(*) FROM items)+(SELECT count(*) FROM embed_queue)"), 0);
}

// Run explicitly as a local scale qualification; it builds a large encrypted scratch database.
TEST(Database, DISABLED_KnowledgeCascadeScale) {
    TempRoot t;
    auto root = DataRoot::open(t.path);
    ASSERT_TRUE(root);
    KeyStore keys(root->keys());
    ASSERT_TRUE(keys.load());
    auto key = keys.get_or_create("cascade-scale.db");
    ASSERT_TRUE(key);
    auto migrations = knowledge::kg_migrations();
    migrations.push_back({knowledge::kKgEvidenceSchemaVersion, knowledge::kg_evidence_migration_sql()});
    MigrationOutcome outcome;
    auto db = open_and_migrate_consistently(root->file("cascade-scale.db"), *key, migrations, outcome, knowledge::register_kg_writer);
    ASSERT_TRUE(db) << db.error().message;
    ASSERT_FALSE(outcome.degraded) << outcome.failure;
    knowledge::KgStore store(std::move(*db));
    const auto number = [&](const std::string& sql) -> long long {
        auto s = store.db().prepare(sql);
        return s.step() ? s.col_int(0) : -1;
    };
    // Background: 10,000 items with 90,000 facts, nine per passage.
    auto setup = store.db().transaction([&]() -> Status {
        return store.db().exec(R"sql(
INSERT INTO entities(id,type,name,key) VALUES(1,'project','A','a'),(2,'date','B','b');
WITH RECURSIVE n(x) AS (VALUES(1) UNION ALL SELECT x+1 FROM n WHERE x<10000)
 INSERT INTO items(id,source,ext_id) SELECT x,'files','source-'||x FROM n;
INSERT INTO chunks(id,item_id,ord,text) SELECT id,id,0,'source passage' FROM items;
INSERT INTO knowledge_revisions(id,item_id,sequence,source_hash,indexed_at,current) SELECT id,id,1,'hash',1,1 FROM items;
INSERT INTO knowledge_evidence(id,revision_id,chunk_id,excerpt,precision) SELECT id,id,id,'source passage','passage' FROM items;
WITH RECURSIVE n(x) AS (VALUES(1) UNION ALL SELECT x+1 FROM n WHERE x<90000)
 INSERT INTO knowledge_facts(id,subject,predicate,object) SELECT x,1,'fact-'||x,2 FROM n;
INSERT INTO knowledge_support_sets(id,fact_id,state) SELECT id,id,'current' FROM knowledge_facts;
INSERT INTO knowledge_support_evidence(support_id,evidence_id) SELECT id,(id-1)/9+1 FROM knowledge_support_sets;
)sql");
    });
    ASSERT_TRUE(setup) << setup.error().message;
    // The large Source: 10,000 passages, each with one extracted finding, written by the real store.
    const auto passages = [](bool revised) {
        std::vector<knowledge::Chunk> chunks;
        for (int n = 1; n <= 10000; ++n)
            chunks.push_back({"", "Passage " + std::to_string(n) + (revised && n % 2 ? " as revised." : " of the large source."), 0});
        return chunks;
    };
    knowledge::ItemInput in;
    in.source = "files";
    in.ext_id = "large";
    in.title = "large";
    in.hash = "h1";
    in.updated_at = 1000;
    in.chunks = passages(false);
    const auto large = store.put_item(in, 2000);
    ASSERT_TRUE(large) << large.error().message;
    setup = store.db().transaction([&]() -> Status {
        auto chunks = store.chunks_of_item(*large);
        int n = 0;
        for (const auto& c : chunks)
            if (!store.add_relation(1, "large-" + std::to_string(++n), 2, 0.9, "2026", "", c.id)) return make_error("storage", "relation");
        return {};
    });
    ASSERT_TRUE(setup) << setup.error().message;
    const long long first_fact = number("SELECT min(id) FROM knowledge_facts WHERE predicate LIKE 'large-%'");
    const long long first_evidence = number("SELECT min(e.id) FROM knowledge_evidence e JOIN chunks c ON c.id=e.chunk_id WHERE c.item_id=" +
                                            std::to_string(*large));
    ASSERT_EQ(number("SELECT count(*) FROM knowledge_facts WHERE predicate LIKE 'large-%' AND id BETWEEN " + std::to_string(first_fact) +
                     " AND " + std::to_string(first_fact + 9999)),
              10000);
    // 300 full-depth derivation chains, 1,000 automatic category assignments and 500 possible inconsistencies
    // all depend on the large Source.
    const std::string f = std::to_string(first_fact), e = std::to_string(first_evidence);
    setup = store.db().transaction([&]() -> Status {
        return store.db().exec(
            "WITH RECURSIVE k(x) AS (VALUES(0) UNION ALL SELECT x+1 FROM k WHERE x<299), l(y) AS (VALUES(1) UNION ALL SELECT y+1 FROM l WHERE y<31) "
            "INSERT INTO knowledge_facts(id,subject,predicate,object) SELECT 200000+x*100+y,1,'derived-'||x||'-'||y,2 FROM k,l;"
            "INSERT INTO knowledge_support_sets(id,fact_id,state) SELECT id,id,'current' FROM knowledge_facts WHERE id>=200000;"
            "INSERT INTO knowledge_support_facts(support_id,required_fact_id) "
            "SELECT id,CASE WHEN id%100=1 THEN " + f + "+(id-200000)/100 ELSE id-1 END FROM knowledge_facts WHERE id>=200000 ORDER BY id DESC;"
            "INSERT INTO knowledge_categories(id,name,normalized_name) VALUES(1,'Projects','projects');"
            "WITH RECURSIVE n(x) AS (VALUES(0) UNION ALL SELECT x+1 FROM n WHERE x<999) "
            "INSERT INTO knowledge_category_assignments(fact_id,category_id,decision,reason,evidence_id) "
            "SELECT s.fact_id,1,'automatic','reason',se.evidence_id FROM n JOIN knowledge_support_sets s ON s.fact_id=" + f + "+x "
            "JOIN knowledge_support_evidence se ON se.support_id=s.id;"
            "WITH RECURSIVE n(x) AS (VALUES(0) UNION ALL SELECT x+1 FROM n WHERE x<499) "
            "INSERT INTO knowledge_conflicts(left_fact,right_fact,left_evidence,right_evidence,explanation,state) "
            "SELECT l.fact_id,r.fact_id,le.evidence_id,re.evidence_id,'differs','open' FROM n "
            "JOIN knowledge_support_sets l ON l.fact_id=" + f + "+2000+2*x JOIN knowledge_support_evidence le ON le.support_id=l.id "
            "JOIN knowledge_support_sets r ON r.fact_id=" + f + "+2001+2*x JOIN knowledge_support_evidence re ON re.support_id=r.id;");
    });
    ASSERT_TRUE(setup) << setup.error().message;
    ASSERT_EQ(number("SELECT count(*) FROM knowledge_support_sets WHERE state IN ('current','unverified')"), 90000 + 10000 + 9300);
    ASSERT_EQ(number("SELECT (SELECT count(*) FROM knowledge_category_assignments)+(SELECT count(*) FROM knowledge_conflicts)"), 1500);
    const auto timed = [](const auto& work) {
        const auto started = std::chrono::steady_clock::now();
        work();
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
    };

    // The interactive target is stated for 1,000 facts: a Source with 1,000 passages and findings, revised by half
    // and then forgotten, in the same store.
    knowledge::ItemInput medium;
    medium.source = "files";
    medium.ext_id = "medium";
    medium.title = "medium";
    medium.hash = "h1";
    medium.updated_at = 1000;
    for (int n = 1; n <= 1000; ++n) medium.chunks.push_back({"", "Medium passage " + std::to_string(n) + ".", 0});
    const auto medium_id = store.put_item(medium, 2000);
    ASSERT_TRUE(medium_id) << medium_id.error().message;
    const auto extract_medium = [&] {
        return store.db().transaction([&]() -> Status {
            for (const auto& c : store.chunks_of_item(*medium_id))
                if (!store.add_relation(1, "medium-" + c.text, 2, 0.9, "2026", "", c.id)) return make_error("storage", "relation");
            return {};
        });
    };
    const auto p95 = [](std::vector<long long> samples) {
        std::sort(samples.begin(), samples.end());
        return samples[(samples.size() * 95 + 99) / 100 - 1];  // nearest rank
    };
    std::vector<long long> medium_revisions;
    for (int sample = 0; sample < 20; ++sample) {
        ASSERT_TRUE(extract_medium());
        // Alternately replace the even and the odd passages.
        medium.hash = "m" + std::to_string(sample);
        for (int n = sample % 2; n < 1000; n += 2) medium.chunks[static_cast<size_t>(n)].text += " Revised.";
        bool ok = false;
        medium_revisions.push_back(timed([&] { ok = static_cast<bool>(store.put_item(medium, 3000 + sample)); }));
        ASSERT_TRUE(ok);
    }
    ASSERT_TRUE(extract_medium());
    bool medium_ok = false;
    const auto medium_forget_ms = timed([&] { medium_ok = store.remove_item("files", "medium", true); });
    ASSERT_TRUE(medium_ok);
    ASSERT_EQ(number("SELECT count(*) FROM knowledge_support_sets WHERE state IN ('current','unverified')"), 90000 + 10000 + 9300);
    std::cout << "[ knowledge cascade ] one Source with 1000 supports, 20 revisions: p95 " << p95(medium_revisions) << " ms, max "
              << *std::max_element(medium_revisions.begin(), medium_revisions.end()) << " ms; forget " << medium_forget_ms << " ms\n";

    // Another writer keeps making small changes while the large Source changes; its waits are what a user notices.
    std::atomic<bool> probing{true};
    std::atomic<int> phase{0};  // which operation a wait overlapped, to attribute the longest ones
    const char* const phases[] = {"withdrawal and staging", "switch", "cleanup", "forget"};
    std::vector<long long> waits;
    std::map<int, long long> longest;  // by phase
    std::thread probe([&] {
        int n = 0;
        while (probing) {
            const int during = phase;
            const auto started = std::chrono::steady_clock::now();
            store.set_meta("probe", std::to_string(++n));
            const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
            waits.push_back(ms);
            longest[std::max(during, static_cast<int>(phase))] = std::max(longest[std::max(during, static_cast<int>(phase))], ms);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    });

    // A revision that replaces half of the passages and keeps the other half.
    in.hash = "h2";
    in.chunks = passages(true);
    Result<long long> revised = make_error("storage", "not run");
    const auto withdrawal_ms = timed([&] { ASSERT_TRUE(store.prepare_item_change(in)); });
    phase = 1;
    const auto revision_ms = timed([&] { revised = store.put_prepared_item(in, 3000); });
    phase = 2;
    const auto cleanup_ms = timed([&] { ASSERT_TRUE(store.finish_item_changes()); });
    phase = 3;
    std::cout << "[ knowledge cascade ] large revision: stepped withdrawal and staging " << withdrawal_ms << " ms, switch " << revision_ms
              << " ms, stepped cleanup " << cleanup_ms << " ms\n";
    ASSERT_TRUE(revised) << revised.error().message;
    EXPECT_EQ(number("SELECT count(*) FROM knowledge_support_sets WHERE state='superseded'"), 5000);
    EXPECT_EQ(number("SELECT count(*) FROM knowledge_support_sets WHERE state IN ('current','unverified')"), 90000);
    EXPECT_EQ(number("SELECT count(*) FROM knowledge_support_sets WHERE id>=200000 AND state='unsupported'"), 9300);
    EXPECT_EQ(number("SELECT count(*) FROM knowledge_conflicts WHERE state='needs_recheck'"), 500);

    bool removed = false;
    const auto forget_ms = timed([&] { removed = store.remove_item("files", "large", true); });
    probing = false;
    probe.join();
    ASSERT_TRUE(removed);
    std::cout << "[ knowledge cascade ] concurrent small writes during the large revision and forget: " << waits.size()
              << " samples, p95 " << p95(waits) << " ms, max " << *std::max_element(waits.begin(), waits.end()) << " ms; longest wait during";
    for (const auto& [during, ms] : longest) std::cout << " " << phases[during] << " " << ms << " ms;";
    std::cout << "\n";
    EXPECT_EQ(number("SELECT count(*) FROM knowledge_facts"), 90000);
    EXPECT_EQ(number("SELECT count(*) FROM knowledge_support_sets"), 90000);
    EXPECT_EQ(number("SELECT (SELECT count(*) FROM knowledge_category_assignments)+(SELECT count(*) FROM knowledge_conflicts)"), 0);
    std::cout << "[ knowledge cascade ] encrypted 10000-item / 109300-fact store, one Source with 10000 supports: revision "
              << revision_ms << " ms, forget " << forget_ms << " ms\n";
}

TEST(Database, KnowledgeConflictDecisionRechecksEvidenceAndForgetsHistory) {
    TempRoot t;
    auto root = DataRoot::open(t.path);
    ASSERT_TRUE(root);
    KeyStore keys(root->keys());
    ASSERT_TRUE(keys.load());
    auto key = keys.get_or_create("review.db");
    ASSERT_TRUE(key);
    auto migrations = knowledge::kg_migrations();
    migrations.push_back({knowledge::kKgEvidenceSchemaVersion, knowledge::kg_evidence_migration_sql()});
    MigrationOutcome outcome;
    auto db = open_and_migrate_consistently(root->file("review.db"), *key, migrations, outcome, knowledge::register_kg_writer);
    ASSERT_TRUE(db) << db.error().message;
    ASSERT_FALSE(outcome.degraded) << outcome.failure;
    knowledge::KgStore store(std::move(*db));
    ASSERT_TRUE(store.db().exec(R"sql(
INSERT INTO entities(id,type,name,key) VALUES(1,'project','Project','project'),(2,'date','June','june'),(3,'date','July','july');
INSERT INTO knowledge_facts(id,subject,predicate,object,valid_from) VALUES(1,1,'deadline',2,'2026'),(2,1,'deadline',3,'2026');
INSERT INTO items(id,source,ext_id,hash) VALUES(1,'files','draft','v1'),(2,'files','plan','v1'),(3,'files','revised','v2');
INSERT INTO chunks(id,item_id,ord,text) VALUES(1,1,0,'June'),(2,2,0,'July'),(3,3,0,'June');
INSERT INTO knowledge_revisions(id,item_id,sequence,source_hash,indexed_at,current)
 VALUES(1,1,1,'v1',1,1),(2,2,1,'v1',1,1),(3,3,1,'v2',1,1);
INSERT INTO knowledge_evidence(id,revision_id,chunk_id,excerpt,precision,byte_start,byte_end)
 VALUES(1,1,1,'June','exact',0,4),(2,2,2,'July','exact',0,4),(3,3,3,'June','exact',0,4);
INSERT INTO knowledge_support_sets(id,fact_id,state) VALUES(1,1,'current'),(2,2,'current'),(3,1,'current');
INSERT INTO knowledge_support_evidence(support_id,evidence_id) VALUES(1,1),(2,2),(3,3);
)sql"));
    auto generation = store.knowledge_generation();
    ASSERT_TRUE(generation);
    EXPECT_FALSE(store.record_conflict(1, 2, 1, 999, "different dates", *generation));
    auto candidate = store.record_conflict(2, 1, 2, 1, "different dates", *generation);
    ASSERT_TRUE(candidate) << candidate.error().message;
    auto rows = store.conflicts(10);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].left_fact, 1);
    EXPECT_EQ(rows[0].left_evidence, 1);
    EXPECT_FALSE(store.decide_conflict(*candidate, "prefer_right", "approved plan", 1, *generation));
    generation = store.knowledge_generation();
    ASSERT_TRUE(generation);
    ASSERT_TRUE(store.decide_conflict(*candidate, "prefer_right", "approved plan", 1, *generation));
    ASSERT_EQ(store.conflict_history(*candidate).size(), 1u);
    generation = store.knowledge_generation();
    ASSERT_TRUE(generation);
    ASSERT_TRUE(store.record_conflict(1, 2, 1, 2, "same evidence", *generation));
    rows = store.conflicts(10);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].state, "resolved");
    EXPECT_EQ(rows[0].explanation, "different dates");
    ASSERT_TRUE(store.db().exec("UPDATE knowledge_revisions SET current=0 WHERE id=1"));
    rows = store.conflicts(10);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].state, "needs_recheck");
    EXPECT_TRUE(rows[0].decision.empty());
    EXPECT_EQ(store.conflict_history(*candidate)[0].reason, "approved plan");
    generation = store.knowledge_generation();
    ASSERT_TRUE(generation);
    EXPECT_FALSE(store.decide_conflict(*candidate, "keep_both", "", 2, *generation));
    ASSERT_TRUE(store.record_conflict(1, 2, 3, 2, "replacement evidence", *generation));
    rows = store.conflicts(10);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].state, "open");
    EXPECT_EQ(rows[0].left_evidence, 3);
    ASSERT_TRUE(store.db().exec("DELETE FROM items WHERE id=1"));
    EXPECT_TRUE(store.conflict_history(*candidate).empty());
    ASSERT_EQ(store.conflicts(10).size(), 1u);
    ASSERT_TRUE(store.db().exec("DELETE FROM items WHERE id=3"));
    EXPECT_TRUE(store.conflicts(10).empty());
}

TEST(Database, KnowledgeImpactSnapshotReportsIndependentAndRequiredSupport) {
    TempRoot t;
    auto root = DataRoot::open(t.path);
    ASSERT_TRUE(root);
    KeyStore keys(root->keys());
    ASSERT_TRUE(keys.load());
    auto key = keys.get_or_create("impact.db");
    ASSERT_TRUE(key);
    auto migrations = knowledge::kg_migrations();
    migrations.push_back({knowledge::kKgEvidenceSchemaVersion, knowledge::kg_evidence_migration_sql()});
    MigrationOutcome outcome;
    auto db = open_and_migrate_consistently(root->file("impact.db"), *key, migrations, outcome, knowledge::register_kg_writer);
    ASSERT_TRUE(db) << db.error().message;
    ASSERT_FALSE(outcome.degraded) << outcome.failure;
    knowledge::KgStore store(std::move(*db));
    ASSERT_TRUE(store.db().exec(R"sql(
INSERT INTO entities(id,type,name,key) VALUES(1,'project','A','a'),(2,'date','B','b');
INSERT INTO knowledge_facts(id,subject,predicate,object) VALUES(1,1,'first',2),(2,1,'second',2),(3,1,'derived',2);
INSERT INTO items(id,source,ext_id) VALUES(1,'files','one'),(2,'files','two'),(3,'files','three');
INSERT INTO chunks(id,item_id,ord,text) VALUES(1,1,0,'one'),(2,2,0,'two'),(3,3,0,'three');
INSERT INTO knowledge_revisions(id,item_id,sequence,source_hash,indexed_at,current)
 VALUES(1,1,1,'1',1,1),(2,2,1,'2',1,1),(3,3,1,'3',1,1);
INSERT INTO knowledge_evidence(id,revision_id,chunk_id,excerpt,precision)
 VALUES(1,1,1,'one','passage'),(2,2,2,'two','passage'),(3,3,3,'three','passage');
INSERT INTO knowledge_support_sets(id,fact_id,state) VALUES(1,1,'current'),(2,2,'current'),(3,3,'current'),(4,3,'current');
INSERT INTO knowledge_support_evidence(support_id,evidence_id) VALUES(1,1),(2,2),(4,3);
INSERT INTO knowledge_support_facts(support_id,required_fact_id) VALUES(3,1),(3,2);
)sql"));
    auto preview = store.preview_evidence_removal({1});
    ASSERT_TRUE(preview) << preview.error().message;
    EXPECT_FALSE(preview->complete);
    ASSERT_EQ(preview->facts.size(), 3u);
    EXPECT_EQ(preview->facts[0].before, 1);
    EXPECT_EQ(preview->facts[0].after, 0);
    EXPECT_EQ(preview->facts[2].before, 2);
    EXPECT_EQ(preview->facts[2].after, 1);
    const auto old_generation = preview->generation;
    const auto item_preview = store.preview_item_removal(1);
    ASSERT_TRUE(item_preview) << item_preview.error().message;
    EXPECT_EQ(item_preview->generation, old_generation);
    EXPECT_EQ(item_preview->facts[0].after, 0);
    EXPECT_EQ(item_preview->facts[2].after, 1);
    EXPECT_FALSE(store.preview_item_removal(999));
    preview = store.preview_evidence_removal({1, 3});
    ASSERT_TRUE(preview);
    EXPECT_EQ(preview->facts[2].after, 0);
    EXPECT_FALSE(store.preview_evidence_removal({999}));
    ASSERT_TRUE(store.db().exec("UPDATE knowledge_facts SET scope='changed' WHERE id=1"));
    ASSERT_TRUE(store.knowledge_generation());
    EXPECT_NE(*store.knowledge_generation(), old_generation);
    ASSERT_TRUE(store.db().exec("INSERT INTO shares(source,ext_id,remote_id,title,groups,state) "
                                "VALUES('files','one','remote-1','private title','[\"group\"]','shared')"));
    auto prepared = store.prepare_item_removal(1, 1000);
    ASSERT_TRUE(prepared) << prepared.error().message;
    EXPECT_FALSE(prepared->impact.complete);
    EXPECT_FALSE(store.confirm_item_removal(prepared->token, false, 1001));
    EXPECT_TRUE(store.item(1));
    ASSERT_TRUE(store.db().exec("UPDATE items SET title='changed after preview' WHERE id=1"));
    EXPECT_FALSE(store.confirm_item_removal(prepared->token, true, 1001));
    EXPECT_TRUE(store.item(1));
    prepared = store.prepare_item_removal(1, 2000);
    ASSERT_TRUE(prepared);
    ASSERT_TRUE(store.cancel_item_removal(prepared->token));
    EXPECT_FALSE(store.confirm_item_removal(prepared->token, true, 2001));
    prepared = store.prepare_item_removal(1, 3000);
    ASSERT_TRUE(prepared);
    EXPECT_FALSE(store.confirm_item_removal(prepared->token, true, 603000));
    EXPECT_TRUE(store.item(1));
    ASSERT_TRUE(store.db().exec(R"sql(
WITH RECURSIVE n(x) AS (VALUES(1) UNION ALL SELECT x+1 FROM n WHERE x<256)
 INSERT INTO knowledge_previews(token,operation,item_id,generation,expires_at)
 SELECT printf('%032d',x),'forget_item',1,0,900000 FROM n;
)sql"));
    const auto full = store.prepare_item_removal(1, 603000);
    ASSERT_FALSE(full);
    EXPECT_EQ(full.error().code, "capacity");
    ASSERT_TRUE(store.db().exec("DELETE FROM knowledge_previews WHERE expires_at=900000"));
    prepared = store.prepare_item_removal(1, 603001);
    ASSERT_TRUE(prepared);
    ASSERT_TRUE(store.confirm_item_removal(prepared->token, true, 603002));
    EXPECT_FALSE(store.item(1));
    EXPECT_FALSE(store.confirm_item_removal(prepared->token, true, 603003));
    auto persisted = store.db().prepare("SELECT (SELECT count(*) FROM knowledge_facts WHERE id=3),"
                                        "(SELECT count(*) FROM user_exclusions WHERE kind='item' AND value='files:one'),"
                                        "(SELECT count(*) FROM shares WHERE ext_id='withdrawal:remote-1' AND title='' AND state='unsharing')");
    ASSERT_TRUE(persisted.step());
    EXPECT_EQ(persisted.col_int(0), 1);
    EXPECT_EQ(persisted.col_int(1), 1);
    EXPECT_EQ(persisted.col_int(2), 1);
}

// Run explicitly as a local scale qualification; it builds a large encrypted scratch database.
TEST(Database, DISABLED_KnowledgeImpactScale) {
    TempRoot t;
    auto root = DataRoot::open(t.path);
    ASSERT_TRUE(root);
    KeyStore keys(root->keys());
    ASSERT_TRUE(keys.load());
    auto key = keys.get_or_create("impact-scale.db");
    ASSERT_TRUE(key);
    auto migrations = knowledge::kg_migrations();
    migrations.push_back({knowledge::kKgEvidenceSchemaVersion, knowledge::kg_evidence_migration_sql()});
    MigrationOutcome outcome;
    auto db = open_and_migrate_consistently(root->file("impact-scale.db"), *key, migrations, outcome, knowledge::register_kg_writer);
    ASSERT_TRUE(db) << db.error().message;
    ASSERT_FALSE(outcome.degraded) << outcome.failure;
    knowledge::KgStore store(std::move(*db));
    const auto setup = store.db().transaction([&]() -> Status {
        return store.db().exec(R"sql(
INSERT INTO entities(id,type,name,key) VALUES(1,'project','A','a'),(2,'date','B','b');
WITH RECURSIVE n(x) AS (VALUES(1) UNION ALL SELECT x+1 FROM n WHERE x<10000)
 INSERT INTO items(id,source,ext_id) SELECT x,'files','source-'||x FROM n;
INSERT INTO chunks(id,item_id,ord,text) SELECT id,id,0,'source passage' FROM items;
INSERT INTO knowledge_revisions(id,item_id,sequence,source_hash,indexed_at,current) SELECT id,id,1,'hash',1,1 FROM items;
INSERT INTO knowledge_evidence(id,revision_id,chunk_id,excerpt,precision)
 SELECT id,id,id,'source passage','passage' FROM items;
WITH RECURSIVE n(x) AS (VALUES(1) UNION ALL SELECT x+1 FROM n WHERE x<100000)
 INSERT INTO knowledge_facts(id,subject,predicate,object) SELECT x,1,'fact-'||x,2 FROM n;
INSERT INTO knowledge_support_sets(id,fact_id,state) SELECT id,id,'current' FROM knowledge_facts;
INSERT INTO knowledge_support_evidence(support_id,evidence_id)
 SELECT id,(id-1)/25%10000+1 FROM knowledge_support_sets WHERE id%25=1;
WITH RECURSIVE n(x) AS (VALUES(100000) UNION ALL SELECT x-1 FROM n WHERE x>2)
 INSERT INTO knowledge_support_facts(support_id,required_fact_id) SELECT x,x-1 FROM n WHERE x%25<>1 ORDER BY x DESC;
)sql");
    });
    ASSERT_TRUE(setup) << setup.error().message;
    // 4,000 derivation chains of 25 facts, the schema's depth bound being 32; the first chain rests on item 1.
    const auto started = std::chrono::steady_clock::now();
    const auto preview = store.preview_item_removal(1);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
    ASSERT_TRUE(preview) << preview.error().message;
    ASSERT_EQ(preview->facts.size(), 100'000u);
    EXPECT_EQ(preview->facts.front().before, 1);
    EXPECT_EQ(preview->facts.front().after, 0);
    EXPECT_EQ(preview->facts[24].before, 1);
    EXPECT_EQ(preview->facts[24].after, 0);
    EXPECT_EQ(preview->facts.back().before, 1);
    EXPECT_EQ(preview->facts.back().after, 1);
    std::cout << "[ knowledge impact ] encrypted 10000-item / 100000-fact snapshot and diff: " << elapsed.count() << " ms\n";
}

TEST(Database, ForgetWaitsForMigrationWriterBeforeRemovingRecoveryCopy) {
    TempRoot t;
    auto root = DataRoot::open(t.path);
    ASSERT_TRUE(root);
    KeyStore keys(root->keys());
    ASSERT_TRUE(keys.load());
    auto key = keys.get_or_create("forget-lock.db");
    ASSERT_TRUE(key);
    MigrationOutcome initial;
    auto db = knowledge::open_kg_database(root->file("forget-lock.db"), *key, initial);
    ASSERT_TRUE(db);
    ASSERT_TRUE((*db)->exec("INSERT INTO items(source,ext_id,title) VALUES('threads','one','private')"));
    knowledge::KgStore store(std::move(*db));
    sqlite3_busy_timeout(store.db().raw(), 0);
    auto migration = Database::open(root->file("forget-lock.db"), *key);
    ASSERT_TRUE(migration);
    ASSERT_TRUE((*migration)->exec("BEGIN IMMEDIATE"));
    const auto backup = root->file("forget-lock.db.bak");
    std::ofstream(backup, std::ios::binary) << "snapshot-in-progress";
    EXPECT_FALSE(store.forget_thread("one"));
    EXPECT_TRUE(std::filesystem::exists(backup));
    ASSERT_TRUE((*migration)->exec("ROLLBACK"));
    ASSERT_TRUE(store.forget_thread("one"));
    EXPECT_FALSE(std::filesystem::exists(backup));
    EXPECT_FALSE(store.item("threads", "one"));
}

TEST(Database, KnowledgeWriterRefusesNewerSchemaWithoutChangingData) {
    TempRoot t;
    auto root = DataRoot::open(t.path);
    ASSERT_TRUE(root);
    KeyStore keys(root->keys());
    ASSERT_TRUE(keys.load());
    auto key = keys.get_or_create("future.db");
    ASSERT_TRUE(key);
    const std::vector<Migration> newer = {{1, "CREATE TABLE a(x INTEGER); INSERT INTO a VALUES(42);"},
                                          {2, "INSERT INTO a VALUES(99);"}};
    {
        MigrationOutcome o;
        auto db = open_and_migrate(root->file("future.db"), *key, newer, o, true);
        ASSERT_TRUE(db);
    }
    MigrationOutcome old_outcome;
    auto old = open_and_migrate(root->file("future.db"), *key, std::span(newer).first(1), old_outcome, true);
    ASSERT_FALSE(old);
    EXPECT_EQ(old.error().code, "schema_too_new");
    MigrationOutcome current_outcome;
    auto current = open_and_migrate(root->file("future.db"), *key, newer, current_outcome, true);
    ASSERT_TRUE(current);
    auto s = (*current)->prepare("SELECT COUNT(*), SUM(x) FROM a");
    ASSERT_TRUE(s.step());
    EXPECT_EQ(s.col_int(0), 2);
    EXPECT_EQ(s.col_int(1), 141);
}

TEST(Database, KnowledgeWriterBarrierRejectsLegacyConnectionOnCopiedEncryptedDatabase) {
    TempRoot t;
    auto root = DataRoot::open(t.path);
    ASSERT_TRUE(root);
    KeyStore keys(root->keys());
    ASSERT_TRUE(keys.load());
    auto key = keys.get_or_create("kg.db");
    ASSERT_TRUE(key);
    {
        MigrationOutcome outcome;
        auto db = open_and_migrate(root->file("kg.db"), *key, knowledge::kg_migrations(), outcome);
        ASSERT_TRUE(db);
        ASSERT_TRUE((*db)->exec("INSERT INTO items(source,ext_id,title) VALUES('files','one','original');"
                                "INSERT INTO chunks(item_id,ord,text) VALUES(1,0,'evidence');"));
        ASSERT_TRUE((*db)->exec("PRAGMA wal_checkpoint(TRUNCATE);"));
    }
    const auto copy = root->file("kg-copy.db");
    std::filesystem::copy_file(root->file("kg.db"), copy);
    auto modern = Database::open(copy, *key);
    ASSERT_TRUE(modern);
    ASSERT_TRUE(knowledge::register_kg_writer(**modern));
    ASSERT_TRUE((*modern)->transaction([&] { return (*modern)->exec(knowledge::kg_writer_barrier_sql()); }));
    auto count = (*modern)->prepare("SELECT count(*) FROM sqlite_master WHERE type='trigger' AND name LIKE 'skowork_guard_%'");
    ASSERT_TRUE(count.step());
    EXPECT_EQ(count.col_int(0), 69);
    auto legacy = Database::open(copy, *key);
    ASSERT_TRUE(legacy);
    EXPECT_FALSE((*legacy)->exec("UPDATE items SET title='legacy' WHERE id=1"));
    EXPECT_FALSE((*legacy)->exec("DELETE FROM chunks WHERE item_id=1"));
    EXPECT_FALSE((*legacy)->exec("INSERT INTO items(source,ext_id) VALUES('files','two')"));
    EXPECT_FALSE((*legacy)->exec("INSERT INTO documents(path) VALUES('legacy')"));
    EXPECT_FALSE((*legacy)->exec("INSERT INTO meta(key,value) VALUES('legacy','value')"));
    EXPECT_FALSE((*legacy)->exec("INSERT INTO notes(title,text,created_at,updated_at) VALUES('legacy','text',1,1)"));
    ASSERT_TRUE((*modern)->exec("UPDATE items SET title='new writer' WHERE id=1"));
    auto row = (*modern)->prepare("SELECT title,(SELECT count(*) FROM chunks) FROM items WHERE id=1");
    ASSERT_TRUE(row.step());
    EXPECT_EQ(row.col_text(0), "new writer");
    EXPECT_EQ(row.col_int(1), 1);
}

TEST(Database, PrototypeKnowledgeSupportSetsOnCopiedEncryptedDatabase) {
    TempRoot t;
    auto root = DataRoot::open(t.path);
    ASSERT_TRUE(root);
    KeyStore keys(root->keys());
    ASSERT_TRUE(keys.load());
    auto key = keys.get_or_create("kg.db");
    ASSERT_TRUE(key);
    {
        MigrationOutcome outcome;
        auto db = open_and_migrate(root->file("kg.db"), *key, knowledge::kg_migrations(), outcome);
        ASSERT_TRUE(db);
        ASSERT_TRUE((*db)->exec(R"sql(
          INSERT INTO items(id,source,ext_id) VALUES(1,'files','a'),(2,'files','b'),(3,'files','c');
          INSERT INTO chunks(id,item_id,ord,text) VALUES(1,1,0,'A'),(2,2,0,'B'),(3,3,0,'C');
          INSERT INTO entities(id,type,name,key) VALUES(1,'project','Project','project'),(2,'date','May','may');
          INSERT INTO relations(id,subject,predicate,object,confidence,valid_from,chunk_id)
            VALUES(1,1,'starts',2,1,'2026',1),(2,1,'starts',2,1,'2026',3),(3,1,'starts',2,1,'2027',2);
          PRAGMA wal_checkpoint(TRUNCATE);
        )sql"));
    }
    const auto copy = root->file("kg-support-prototype.db");
    std::filesystem::copy_file(root->file("kg.db"), copy);
    auto db = Database::open(copy, *key);
    ASSERT_TRUE(db);
    // Test-only schema. A support set is one proof, and every member of that set is required.
    // Removing any member removes the whole proof, not just the member (which would make it look complete).
    ASSERT_TRUE((*db)->exec(R"sql(
      CREATE TABLE prototype_facts(id INTEGER PRIMARY KEY, subject INTEGER, predicate TEXT, object INTEGER, valid_from TEXT, valid_to TEXT);
      CREATE TABLE prototype_sets(id INTEGER PRIMARY KEY, fact_id INTEGER NOT NULL REFERENCES prototype_facts(id));
      CREATE TABLE prototype_members(set_id INTEGER NOT NULL REFERENCES prototype_sets(id) ON DELETE CASCADE,
                                     chunk_id INTEGER NOT NULL REFERENCES chunks(id), PRIMARY KEY(set_id,chunk_id));
      CREATE TRIGGER prototype_forget BEFORE DELETE ON chunks BEGIN
        DELETE FROM prototype_sets WHERE id IN (SELECT set_id FROM prototype_members WHERE chunk_id=old.id);
      END;
      INSERT INTO prototype_facts(subject,predicate,object,valid_from,valid_to)
        SELECT subject,predicate,object,valid_from,valid_to FROM relations GROUP BY subject,predicate,object,valid_from,valid_to;
      INSERT INTO prototype_sets(id,fact_id)
        SELECT r.id,f.id FROM relations r JOIN prototype_facts f ON f.subject=r.subject AND f.predicate=r.predicate
          AND f.object=r.object AND f.valid_from=r.valid_from AND f.valid_to=r.valid_to;
      INSERT INTO prototype_members(set_id,chunk_id) SELECT id,chunk_id FROM relations;
      -- The first proof requires A and B; C is an independent proof of the same 2026 fact.
      INSERT INTO prototype_members(set_id,chunk_id) VALUES(1,2);
    )sql"));
    const auto count = [&](const char* sql) {
        auto statement = (*db)->prepare(sql);
        return statement.step() ? statement.col_int(0) : -1LL;
    };
    EXPECT_EQ(count("SELECT count(*) FROM prototype_facts"), 2);  // different validity intervals do not merge
    EXPECT_EQ(count("SELECT count(*) FROM prototype_sets s JOIN prototype_facts f ON f.id=s.fact_id WHERE f.valid_from='2026'"), 2);
    ASSERT_TRUE((*db)->exec("DELETE FROM items WHERE id=1"));
    EXPECT_EQ(count("SELECT count(*) FROM prototype_sets WHERE id=1"), 0);  // A and B proof is invalidated
    EXPECT_EQ(count("SELECT count(*) FROM prototype_sets WHERE id=2"), 1);  // C independently supports the fact
    ASSERT_TRUE((*db)->exec("DELETE FROM items WHERE id=3"));
    EXPECT_EQ(count("SELECT count(*) FROM prototype_sets s JOIN prototype_facts f ON f.id=s.fact_id WHERE f.valid_from='2026'"), 0);
    EXPECT_EQ(count("SELECT count(*) FROM prototype_sets s JOIN prototype_facts f ON f.id=s.fact_id WHERE f.valid_from='2027'"), 1);
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
