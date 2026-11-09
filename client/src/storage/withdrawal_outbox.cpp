#include "storage/withdrawal_outbox.h"

#include "base/json.h"
#include "base/encoding.h"
#include "base/strings.h"
#include "platform/crypto.h"
#include "platform/dpapi.h"
#include "platform/win.h"

#include <algorithm>
#include <fstream>
#include <limits>

namespace wb {
namespace {

constexpr wchar_t kPurpose[] = L"knowledge-withdrawal-outbox/v1";
constexpr size_t kMaxBytes = 8 * 1024 * 1024;
constexpr size_t kMaxEntries = 10000;

class OutboxLock {
public:
    explicit OutboxLock(const std::filesystem::path& file) {
        std::error_code ec;
        const auto absolute = std::filesystem::absolute(file, ec);
        if (ec || absolute.filename().empty()) return;
        // Do not open the replaceable file before locking: another writer can be
        // atomically replacing it, and pathname resolution may deny that rename.
        const auto parent = std::filesystem::weakly_canonical(absolute.parent_path(), ec);
        if (ec) return;
        const auto path = parent / absolute.filename();
        const auto digest = sha256(to_lower(narrow(path.wstring())));
        if (digest.size() != 32) return;
        mutex_.reset(::CreateMutexW(nullptr, FALSE, widen("Global\\SkoWorkWithdrawal-" + base64url_encode(digest)).c_str()));
        if (!mutex_) return;
        const auto result = ::WaitForSingleObject(mutex_.get(), 30000);
        acquired_ = result == WAIT_OBJECT_0 || result == WAIT_ABANDONED;
    }
    ~OutboxLock() { if (acquired_) ::ReleaseMutex(mutex_.get()); }
    explicit operator bool() const { return acquired_; }
private:
    unique_handle mutex_;
    bool acquired_ = false;
};

bool valid(const WithdrawalIdentity& e) {
    const auto bounded = [](const std::string& s) { return !s.empty() && s.size() <= 2048; };
    const auto id_char = [](unsigned char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                                                   (c >= '0' && c <= '9') || c == '-' || c == '_'; };
    return bounded(e.issuer) && bounded(e.endpoint) && bounded(e.subject) && bounded(e.remote_id) &&
           e.kind == "item" &&
           std::all_of(e.remote_id.begin(), e.remote_id.end(), id_char) && e.expected_revision >= 0;
}

}  // namespace

Result<WithdrawalState> WithdrawalOutbox::load() const {
    const OutboxLock lock(file_);
    if (!lock) return make_error("outbox", "Cannot lock pending remote removals.");
    return load_unlocked();
}

Result<WithdrawalState> WithdrawalOutbox::load_unlocked() const try {
    const DWORD attributes = ::GetFileAttributesW(file_.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)))
        return make_error("outbox", "Pending remote removals are not a regular file.");
    std::error_code ec;
    const bool exists = std::filesystem::exists(file_, ec);
    if (ec) return make_error("outbox", "Cannot inspect pending remote removals.");
    if (!exists) return WithdrawalState{};
    const auto size = std::filesystem::file_size(file_, ec);
    if (ec || size == 0 || size > kMaxBytes) return make_error("outbox", "Pending remote removals are unreadable.");
    std::ifstream in(file_, std::ios::binary);
    bytes encrypted(static_cast<size_t>(size));
    if (!in.read(reinterpret_cast<char*>(encrypted.data()), static_cast<std::streamsize>(encrypted.size())))
        return make_error("outbox", "Cannot read pending remote removals.");
    auto plain = dpapi_unprotect(encrypted, kPurpose);
    if (!plain) return make_error("outbox", "Cannot decrypt pending remote removals.");
    const auto parsed = parse_json(std::string(plain->begin(), plain->end()));
    ::SecureZeroMemory(plain->data(), plain->size());
    if (!parsed || !parsed->is_object() || parsed->size() != 4 || !parsed->contains("version") ||
        !(*parsed)["version"].is_number_integer() || (*parsed)["version"] != 1 ||
        !parsed->contains("revision") || !(*parsed)["revision"].is_number_integer() ||
        !json_int((*parsed)["revision"]) || *json_int((*parsed)["revision"]) <= 0 ||
        !parsed->contains("wipe_pending") || !(*parsed)["wipe_pending"].is_boolean() ||
        !parsed->contains("entries") || !(*parsed)["entries"].is_array() || (*parsed)["entries"].size() > kMaxEntries)
        return make_error("outbox", "Pending remote removals have an unsupported format.");
    WithdrawalState out;
    out.wipe_pending = (*parsed)["wipe_pending"].get<bool>();
    out.revision = *json_int((*parsed)["revision"]);
    for (const auto& j : (*parsed)["entries"]) {
        if (!j.is_object() || j.size() != 6 || !j.contains("issuer") || !j["issuer"].is_string() ||
            !j.contains("endpoint") || !j["endpoint"].is_string() || !j.contains("subject") || !j["subject"].is_string() ||
            !j.contains("kind") || !j["kind"].is_string() || !j.contains("remote_id") || !j["remote_id"].is_string() ||
            !j.contains("expected_revision") || !j["expected_revision"].is_number_integer() || !json_int(j["expected_revision"]))
            return make_error("outbox", "Pending remote removals have an unsupported record.");
        WithdrawalIdentity e{j["issuer"].get<std::string>(), j["endpoint"].get<std::string>(), j["subject"].get<std::string>(),
                             j["kind"].get<std::string>(), j["remote_id"].get<std::string>(), *json_int(j["expected_revision"])};
        if (!valid(e)) return make_error("outbox", "Pending remote removals have an invalid identity.");
        out.entries.push_back(std::move(e));
    }
    return out;
} catch (const std::exception&) {
    return make_error("outbox", "Pending remote removals have an invalid format.");
}

Status WithdrawalOutbox::save(const WithdrawalState& state) const try {
    const OutboxLock lock(file_);
    if (!lock) return make_error("outbox", "Cannot lock pending remote removals.");
    auto existing = load_unlocked();
    if (!existing) return existing.error();
    if (state.revision != existing->revision || state.revision == std::numeric_limits<long long>::max())
        return make_error("stale", "Pending remote removals changed; reload them and try again.");
    if (state.entries.size() > kMaxEntries) return make_error("outbox", "Too many pending remote removals.");
    json entries = json::array();
    for (const auto& e : state.entries) {
        if (!valid(e)) return make_error("outbox", "A pending remote removal has an invalid identity.");
        entries.push_back({{"issuer", e.issuer}, {"endpoint", e.endpoint}, {"subject", e.subject}, {"kind", e.kind},
                           {"remote_id", e.remote_id}, {"expected_revision", e.expected_revision}});
    }
    std::string plain = json{{"version", 1}, {"revision", state.revision + 1}, {"wipe_pending", state.wipe_pending},
                             {"entries", std::move(entries)}}.dump();
    if (plain.size() > kMaxBytes / 2) return make_error("outbox", "Pending remote removals exceed the storage limit.");
    bytes input(plain.begin(), plain.end());
    auto encrypted = dpapi_protect(input, kPurpose);
    ::SecureZeroMemory(input.data(), input.size());
    ::SecureZeroMemory(plain.data(), plain.size());
    if (!encrypted || encrypted->size() > kMaxBytes) return make_error("outbox", "Cannot protect pending remote removals.");
    const auto suffix = random_urlsafe(12);
    if (suffix.empty()) return make_error("outbox", "Cannot create pending removal staging file.");
    auto temporary = file_;
    temporary += widen("." + suffix + ".tmp");
    unique_file file(::CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file) return make_error("outbox", "Cannot create pending removal staging file.");
    DWORD written = 0;
    const bool write_ok = ::WriteFile(file.get(), encrypted->data(), static_cast<DWORD>(encrypted->size()), &written, nullptr);
    const bool short_write = write_ok && written != encrypted->size();
    const bool ok = write_ok && !short_write && ::FlushFileBuffers(file.get());
    const DWORD write_error = ok ? ERROR_SUCCESS : short_write ? ERROR_WRITE_FAULT : ::GetLastError();
    file.reset();
    if (!ok) {
        (void)::DeleteFileW(temporary.c_str());
        return make_error("outbox", "Cannot write pending remote removals (Windows error " + std::to_string(write_error) + ").");
    }
    if (!::MoveFileExW(temporary.c_str(), file_.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD move_error = ::GetLastError();
        (void)::DeleteFileW(temporary.c_str());
        return make_error("outbox", "Cannot commit pending remote removals (Windows error " + std::to_string(move_error) + ").");
    }
    return {};
} catch (const std::exception&) {
    return make_error("outbox", "Cannot encode pending remote removals.");
}

Status WithdrawalOutbox::finish_interrupted_wipe(const std::function<Status()>& wipe) const {
    const OutboxLock lock(file_);
    if (!lock) return make_error("outbox", "Cannot lock pending remote removals.");
    auto state = load_unlocked();
    if (!state) return state.error();
    if (!state->wipe_pending) return {};
    if (auto result = wipe(); !result) return result;
    state->wipe_pending = false;
    // Windows mutexes are recursive for the owning thread; save rechecks the revision while the outer lock is held.
    return save(*state);
}

Result<bool> WithdrawalOutbox::has_item(const std::string& issuer, const std::string& endpoint, const std::string& subject,
                                        const std::string& remote_id) const {
    if (issuer.empty() || endpoint.empty() || subject.empty() || remote_id.empty())
        return make_error("outbox", "A signed-in account is required to check pending remote removals.");
    const auto state = load();
    if (!state) return state.error();
    if (state->wipe_pending) return true;
    return std::any_of(state->entries.begin(), state->entries.end(), [&](const WithdrawalIdentity& entry) {
        return entry.kind == "item" && entry.issuer == issuer && entry.endpoint == endpoint &&
               entry.subject == subject && entry.remote_id == remote_id;
    });
}

Result<size_t> WithdrawalOutbox::retry_items(const std::string& issuer, const std::string& endpoint, const std::string& subject,
                                            const std::function<Status(const std::string&)>& remove) const {
    if (issuer.empty() || endpoint.empty() || subject.empty() || !remove)
        return make_error("outbox", "A signed-in account is required for remote removal.");
    size_t completed = 0;
    while (completed < 50) {
        auto state = load();
        if (!state) return state.error();
        if (state->wipe_pending) return make_error("outbox", "Local deletion must finish before remote removal resumes.");
        const auto it = std::find_if(state->entries.begin(), state->entries.end(), [&](const WithdrawalIdentity& entry) {
            return entry.kind == "item" && entry.issuer == issuer && entry.endpoint == endpoint && entry.subject == subject;
        });
        if (it == state->entries.end()) break;
        const size_t index = static_cast<size_t>(it - state->entries.begin());
        if (auto result = remove(it->remote_id); !result) return result.error();
        state->entries.erase(state->entries.begin() + static_cast<std::ptrdiff_t>(index));
        // If another writer changed the outbox during the request, keep the record and retry the idempotent removal.
        if (auto result = save(*state); !result) return result.error();
        ++completed;
    }
    return completed;
}

}  // namespace wb
