// Encrypted SQLite (SQLite3MultipleCiphers, SQLCipher v4 scheme, raw key; design D6) with prepared statements,
// transactions and a migration runner that backs up and rolls back (client/local-storage Schema Migration).
#pragma once

#include "base/result.h"
#include "storage/keystore.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

struct sqlite3;
struct sqlite3_stmt;

namespace wb {

class Database;

class Statement {
public:
    Statement(Statement&& o) noexcept;
    Statement& operator=(Statement&&) = delete;
    Statement(const Statement&) = delete;
    ~Statement();

    Statement& bind(int idx, long long v);
    Statement& bind(int idx, int v) { return bind(idx, static_cast<long long>(v)); }
    Statement& bind(int idx, double v);
    Statement& bind(int idx, std::string_view v);
    Statement& bind(int idx, const char* v) { return bind(idx, std::string_view(v)); }
    Statement& bind(int idx, const std::string& v) { return bind(idx, std::string_view(v)); }
    Statement& bind_blob(int idx, const std::vector<std::uint8_t>& v);
    Statement& bind_null(int idx);
    Statement& bind(int idx, const std::optional<std::string>& v) { return v ? bind(idx, *v) : bind_null(idx); }

    // Returns true while a row is available; false at the end. Errors are recorded and returned by status().
    bool step();
    Status run();  // steps to completion
    void reset();

    [[nodiscard]] long long col_int(int i) const;
    [[nodiscard]] double col_double(int i) const;
    [[nodiscard]] std::string col_text(int i) const;
    [[nodiscard]] std::vector<std::uint8_t> col_blob(int i) const;
    [[nodiscard]] bool col_null(int i) const;
    [[nodiscard]] const Status& status() const { return status_; }

private:
    friend class Database;
    Statement(Database* db, sqlite3_stmt* s) : db_(db), s_(s) {}
    Database* db_;
    sqlite3_stmt* s_;
    Status status_;
};

struct Migration {
    int version;
    std::string sql;
};

class Database {
public:
    // Opens or creates `file` encrypted with `key`. Fails with "not_a_database" for a wrong key.
    static Result<std::unique_ptr<Database>> open(const std::filesystem::path& file, const db_key& key,
                                                  bool read_only = false);
    ~Database();
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    Status exec(std::string_view sql);
    Statement prepare(std::string_view sql);
    // Runs fn inside BEGIN IMMEDIATE ... COMMIT; rolls back when fn fails (client/local-storage Crash-Safe Writes).
    Status transaction(const std::function<Status()>& fn);
    [[nodiscard]] long long last_insert_id() const;
    [[nodiscard]] int changes() const;
    [[nodiscard]] int user_version();
    [[nodiscard]] bool read_only() const { return read_only_; }
    [[nodiscard]] const std::filesystem::path& path() const { return path_; }
    [[nodiscard]] sqlite3* raw() const { return db_; }
    // Serialises multi-statement sequences across threads (SQLite itself is opened in serialized mode).
    [[nodiscard]] std::recursive_mutex& mutex() { return mx_; }
    std::string errmsg() const;

private:
    Database() = default;
    sqlite3* db_ = nullptr;
    bool read_only_ = false;
    std::filesystem::path path_;
    std::recursive_mutex mx_;
};

struct MigrationOutcome {
    bool migrated = false;     // at least one migration ran
    bool degraded = false;     // a migration failed: the backup was restored and the database is read-only
    int from_version = 0;
    int to_version = 0;
    std::string failure;       // failed migration detail (no secrets)
};

// Opens `file`, applies pending `migrations` in version order after taking a backup copy, and on failure restores
// the copy and reopens read-only (client/local-storage Schema Migration).
Result<std::unique_ptr<Database>> open_and_migrate(const std::filesystem::path& file, const db_key& key,
                                                   std::span<const Migration> migrations, MigrationOutcome& outcome,
                                                   bool refuse_newer = false,
                                                   const std::function<Status(Database&)>& configure = {});

// For knowledge: hold a writer reservation across an encrypted snapshot and all pending migrations. A failure
// rolls back the whole migration instead of replacing a live database file from another connection.
Result<std::unique_ptr<Database>> open_and_migrate_consistently(const std::filesystem::path& file, const db_key& key,
                                                              std::span<const Migration> migrations, MigrationOutcome& outcome,
                                                              const std::function<Status(Database&)>& configure = {});

}  // namespace wb
