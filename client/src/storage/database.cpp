#include "storage/database.h"

#include "base/encoding.h"
#include "platform/win.h"

#include <sqlite3mc_amalgamation.h>

#include <algorithm>

namespace wb {

// ------------------------------------------------------------------------------------------ Statement

Statement::Statement(Statement&& o) noexcept : db_(o.db_), s_(std::exchange(o.s_, nullptr)), status_(o.status_) {}

Statement::~Statement() {
    if (s_) sqlite3_finalize(s_);
}

Statement& Statement::bind(int idx, long long v) {
    if (s_) sqlite3_bind_int64(s_, idx, v);
    return *this;
}

Statement& Statement::bind(int idx, double v) {
    if (s_) sqlite3_bind_double(s_, idx, v);
    return *this;
}

Statement& Statement::bind(int idx, std::string_view v) {
    if (s_) sqlite3_bind_text64(s_, idx, v.data(), v.size(), SQLITE_TRANSIENT, SQLITE_UTF8);
    return *this;
}

Statement& Statement::bind_blob(int idx, const std::vector<std::uint8_t>& v) {
    if (s_) sqlite3_bind_blob64(s_, idx, v.data(), v.size(), SQLITE_TRANSIENT);
    return *this;
}

Statement& Statement::bind_null(int idx) {
    if (s_) sqlite3_bind_null(s_, idx);
    return *this;
}

bool Statement::step() {
    if (!s_) return false;
    const int rc = sqlite3_step(s_);
    if (rc == SQLITE_ROW) return true;
    if (rc != SQLITE_DONE) status_ = make_error("sqlite", db_->errmsg());
    return false;
}

Status Statement::run() {
    if (!s_) return status_.ok() ? Status(make_error("sqlite", "statement not prepared")) : status_;
    while (step()) {
    }
    return status_;
}

void Statement::reset() {
    if (!s_) return;
    sqlite3_reset(s_);
    sqlite3_clear_bindings(s_);
    status_ = {};
}

long long Statement::col_int(int i) const { return sqlite3_column_int64(s_, i); }
double Statement::col_double(int i) const { return sqlite3_column_double(s_, i); }

std::string Statement::col_text(int i) const {
    const auto* p = reinterpret_cast<const char*>(sqlite3_column_text(s_, i));
    return p ? std::string(p, static_cast<size_t>(sqlite3_column_bytes(s_, i))) : std::string{};
}

std::vector<std::uint8_t> Statement::col_blob(int i) const {
    const auto* p = static_cast<const std::uint8_t*>(sqlite3_column_blob(s_, i));
    return p ? std::vector<std::uint8_t>(p, p + sqlite3_column_bytes(s_, i)) : std::vector<std::uint8_t>{};
}

bool Statement::col_null(int i) const { return sqlite3_column_type(s_, i) == SQLITE_NULL; }

// ------------------------------------------------------------------------------------------ Database

Result<std::unique_ptr<Database>> Database::open(const std::filesystem::path& file, const db_key& key, bool read_only) {
    std::unique_ptr<Database> d(new Database());
    d->path_ = file;
    d->read_only_ = read_only;
    const std::string utf8 = narrow(file.wstring());
    const int flags = (read_only ? SQLITE_OPEN_READONLY : SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE) |
                      SQLITE_OPEN_FULLMUTEX;
    if (sqlite3_open_v2(utf8.c_str(), &d->db_, flags, nullptr) != SQLITE_OK) {
        Error e = make_error("sqlite", d->db_ ? sqlite3_errmsg(d->db_) : "sqlite3_open_v2 failed");
        return e;
    }
    // SQLCipher v4 scheme for this connection, independent of the build default.
    const int cipher = sqlite3mc_cipher_index("sqlcipher");
    if (cipher <= 0 || sqlite3mc_config(d->db_, "cipher", cipher) != cipher ||
        sqlite3mc_config_cipher(d->db_, "sqlcipher", "legacy", 4) != 4)
        return make_error("sqlite", "cannot select the sqlcipher v4 scheme");
    // Raw key: x'<64 hex>' skips PBKDF2 because the key is already random.
    std::string raw = "x'" + hex_encode(key.data(), key.size()) + "'";
    const int rc = sqlite3_key_v2(d->db_, "main", raw.data(), static_cast<int>(raw.size()));
    ::SecureZeroMemory(raw.data(), raw.size());
    if (rc != SQLITE_OK) return make_error("sqlite", d->errmsg());
    // Touch the schema: a wrong key fails here with SQLITE_NOTADB.
    char* msg = nullptr;
    if (sqlite3_exec(d->db_, "SELECT count(*) FROM sqlite_master;", nullptr, nullptr, &msg) != SQLITE_OK) {
        const int ext = sqlite3_extended_errcode(d->db_);
        sqlite3_free(msg);
        return make_error(ext == SQLITE_NOTADB ? "not_a_database" : "sqlite", d->errmsg());
    }
    sqlite3_busy_timeout(d->db_, 5000);
    if (!read_only) {
        if (auto s = d->exec("PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL; PRAGMA foreign_keys=ON;"); !s)
            return s.error();
    }
    return d;
}

Database::~Database() {
    if (db_) sqlite3_close_v2(db_);
}

std::string Database::errmsg() const { return db_ ? sqlite3_errmsg(db_) : "no database"; }

Status Database::exec(std::string_view sql) {
    const std::lock_guard lock(mx_);
    char* msg = nullptr;
    const std::string s(sql);
    if (sqlite3_exec(db_, s.c_str(), nullptr, nullptr, &msg) != SQLITE_OK) {
        Error e = make_error("sqlite", msg ? msg : errmsg());
        sqlite3_free(msg);
        return e;
    }
    return {};
}

Statement Database::prepare(std::string_view sql) {
    sqlite3_stmt* s = nullptr;
    Statement st(this, nullptr);
    if (sqlite3_prepare_v2(db_, sql.data(), static_cast<int>(sql.size()), &s, nullptr) != SQLITE_OK) {
        st.status_ = make_error("sqlite", errmsg());
        return st;
    }
    st.s_ = s;
    return st;
}

Status Database::transaction(const std::function<Status()>& fn) {
    const std::lock_guard lock(mx_);
    if (auto s = exec("BEGIN IMMEDIATE;"); !s) return s;
    Status result = fn();
    if (result) {
        if (auto c = exec("COMMIT;"); !c) {
            exec("ROLLBACK;");
            return c;
        }
    } else {
        exec("ROLLBACK;");
    }
    return result;
}

long long Database::last_insert_id() const { return sqlite3_last_insert_rowid(db_); }
int Database::changes() const { return sqlite3_changes(db_); }

int Database::user_version() {
    auto st = prepare("PRAGMA user_version;");
    return st.step() ? static_cast<int>(st.col_int(0)) : 0;
}

// ------------------------------------------------------------------------------------------ migrations

Result<std::unique_ptr<Database>> open_and_migrate(const std::filesystem::path& file, const db_key& key,
                                                   std::span<const Migration> migrations, MigrationOutcome& outcome) {
    auto db = Database::open(file, key);
    if (!db) return db;
    std::vector<Migration> ordered(migrations.begin(), migrations.end());
    std::sort(ordered.begin(), ordered.end(), [](const Migration& a, const Migration& b) { return a.version < b.version; });
    outcome.from_version = (*db)->user_version();
    outcome.to_version = outcome.from_version;
    const bool pending = std::any_of(ordered.begin(), ordered.end(),
                                     [&](const Migration& m) { return m.version > outcome.from_version; });
    if (!pending) return db;

    // Backup: checkpoint the WAL into the main file, then copy the (encrypted) file. A fresh database has nothing
    // to protect, so it is not backed up.
    auto backup = file;
    backup += ".bak";
    std::error_code ec;
    const bool has_data = outcome.from_version > 0;
    if (has_data) {
        (*db)->exec("PRAGMA wal_checkpoint(TRUNCATE);");
        std::filesystem::copy_file(file, backup, std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) return make_error("backup", "cannot back up database before migration: " + ec.message());
    }

    for (const auto& m : ordered) {
        if (m.version <= outcome.to_version) continue;
        Status s = (*db)->transaction([&]() -> Status {
            if (auto e = (*db)->exec(m.sql); !e) return e;
            return (*db)->exec("PRAGMA user_version=" + std::to_string(m.version) + ";");
        });
        if (!s) {
            outcome.failure = "migration " + std::to_string(m.version) + ": " + s.error().message;
            outcome.degraded = true;
            db->reset();  // close the connection before the files are replaced
            if (has_data) {
                std::filesystem::remove(file.wstring() + L"-wal", ec);
                std::filesystem::remove(file.wstring() + L"-shm", ec);
                std::filesystem::copy_file(backup, file, std::filesystem::copy_options::overwrite_existing, ec);
                if (ec) return make_error("restore", "restore after failed migration failed: " + ec.message());
            }
            auto ro = Database::open(file, key, /*read_only=*/true);
            if (!ro) return ro;
            outcome.to_version = (*ro)->user_version();
            return ro;
        }
        outcome.to_version = m.version;
        outcome.migrated = true;
    }
    return db;
}

}  // namespace wb
