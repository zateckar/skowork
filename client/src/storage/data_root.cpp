#include "storage/data_root.h"

#include "base/encoding.h"
#include "base/json.h"
#include "base/strings.h"
#include "platform/crypto.h"
#include "platform/win.h"
#include "storage/withdrawal_outbox.h"

#include <shlobj.h>

#include <algorithm>
#include <fstream>
#include <map>
#include <sstream>
#include <vector>

namespace wb {
namespace fs = std::filesystem;
namespace {

constexpr char kBindingFile[] = "binding.json";
constexpr char kBootstrapFile[] = "bootstrap.json";
constexpr char kBootstrapTemp[] = "bootstrap.json.tmp";
constexpr wchar_t kStagingSuffix[] = L".skowork-staging";

std::optional<json> read_json_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return std::nullopt;
    std::stringstream ss;
    ss << f.rdbuf();
    return parse_json(ss.str());
}

bool write_json_file(const fs::path& p, const json& j) {
    auto tmp = p;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f << j.dump(2);
        if (!f) return false;
    }
    std::error_code ec;
    fs::rename(tmp, p, ec);
    return !ec;
}

// Written to a temporary file, flushed and renamed with write-through, so the record survives a power loss before
// a later step relies on it.
Status write_durable(const fs::path& p, const std::string& text) {
    auto tmp = p;
    tmp += ".tmp";
    unique_file file(::CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file) return make_error("data_root", "cannot create " + tmp.filename().string() + ": " + error_text(::GetLastError()));
    DWORD written = 0;
    const bool ok = ::WriteFile(file.get(), text.data(), static_cast<DWORD>(text.size()), &written, nullptr) &&
                    written == text.size() && ::FlushFileBuffers(file.get());
    file.reset();
    if (!ok || !::MoveFileExW(tmp.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD e = ::GetLastError();
        (void)::DeleteFileW(tmp.c_str());
        return make_error("data_root", "cannot write " + p.filename().string() + ": " + error_text(e));
    }
    return {};
}

fs::path profile_dir() {
    wchar_t* p = nullptr;
    fs::path out;
    if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_Profile, KF_FLAG_DEFAULT, nullptr, &p))) out = p;
    ::CoTaskMemFree(p);
    return out;
}

// Case-insensitive, normalized absolute form without a trailing separator, for comparisons only.
std::string key_of(const fs::path& p) {
    std::error_code ec;
    auto s = to_lower(narrow(fs::absolute(p, ec).lexically_normal().wstring()));
    while (s.size() > 3 && (s.back() == '\\' || s.back() == '/')) s.pop_back();
    return s;
}
bool same_path(const fs::path& a, const fs::path& b) { return key_of(a) == key_of(b); }
bool strictly_inside(const fs::path& child, const fs::path& parent) {
    const auto c = key_of(child), p = key_of(parent);
    return !p.empty() && c.size() > p.size() + 1 && starts_with(c, p + "\\");
}

bool is_link(const fs::path& p) {
    const DWORD a = ::GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_REPARSE_POINT);
}
bool exists_no_follow(const fs::path& p) { return ::GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }
bool is_anchor_file(const fs::path& name) {
    const auto n = name.filename().string();
    return n == kBootstrapFile || n == kBootstrapTemp;
}

// The anchor's record (design D2 root transition).
struct Transition {
    fs::path from, to, staging;
    std::string stage;  // copying | moving | switched
};
struct Boot {
    std::optional<fs::path> requested, active;
    std::optional<Transition> transition;
};

Result<Boot> read_boot(const fs::path& anchor) {
    Boot b;
    const auto file = anchor / kBootstrapFile;
    if (!exists_no_follow(file)) return b;
    auto j = read_json_file(file);
    if (!j || !j->is_object()) return make_error("data_root", "the data-root bootstrap record is unreadable");
    if (auto s = jstr(*j, "requested"); !s.empty()) b.requested = fs::path(widen(s));
    if (auto s = jstr(*j, "active"); !s.empty()) b.active = fs::path(widen(s));
    if (j->contains("transition") && (*j)["transition"].is_object()) {
        const auto& t = (*j)["transition"];
        Transition tr{widen(jstr(t, "from")), widen(jstr(t, "to")), widen(jstr(t, "staging")), jstr(t, "stage")};
        if (tr.from.empty() || tr.to.empty() || tr.staging.empty() ||
            (tr.stage != "copying" && tr.stage != "moving" && tr.stage != "switched"))
            return make_error("data_root", "the data-root transition record is invalid");
        b.transition = tr;
    }
    return b;
}

Status write_boot(const fs::path& anchor, const Boot& b) {
    std::string err;
    if (!create_private_directory(anchor.wstring(), err)) return make_error("data_root", err);
    json j = json::object();
    if (b.requested) j["requested"] = narrow(b.requested->wstring());
    if (b.active) j["active"] = narrow(b.active->wstring());
    if (b.transition) {
        const auto& t = *b.transition;
        j["transition"] = json{{"from", narrow(t.from.wstring())}, {"to", narrow(t.to.wstring())},
                               {"staging", narrow(t.staging.wstring())}, {"stage", t.stage}};
    }
    return write_durable(anchor / kBootstrapFile, j.dump(2));
}

// One managed-root operation at a time for this anchor, across processes.
class AnchorLock {
public:
    explicit AnchorLock(const fs::path& anchor) {
        const auto digest = sha256(key_of(anchor));
        if (digest.size() != 32) return;
        mutex_.reset(::CreateMutexW(nullptr, FALSE, widen("Global\\SkoworkDataRoot-" + base64url_encode(digest)).c_str()));
        if (!mutex_) return;
        const auto result = ::WaitForSingleObject(mutex_.get(), 30000);
        acquired_ = result == WAIT_OBJECT_0 || result == WAIT_ABANDONED;
    }
    ~AnchorLock() { if (acquired_) ::ReleaseMutex(mutex_.get()); }
    AnchorLock(const AnchorLock&) = delete;
    AnchorLock& operator=(const AnchorLock&) = delete;
    explicit operator bool() const { return acquired_; }
private:
    unique_handle mutex_;
    bool acquired_ = false;
};

// Relative paths of a tree, not following links. A link or anything other than a file or folder is refused; the
// anchor's own record is left out when `skip_anchor` (the default root holds it).
struct Entry {
    fs::path rel;
    bool dir = false;
    bool operator<(const Entry& o) const { return rel < o.rel; }
    bool operator==(const Entry& o) const { return rel == o.rel && dir == o.dir; }
};
Result<std::vector<Entry>> list_tree(const fs::path& root, bool skip_anchor) {
    std::vector<Entry> out;
    std::error_code ec;
    fs::recursive_directory_iterator it(root, fs::directory_options::none, ec), end;
    if (ec) return make_error("data_root", "cannot read " + narrow(root.wstring()) + ": " + ec.message());
    for (; it != end; it.increment(ec)) {
        if (ec) return make_error("data_root", "cannot read " + narrow(root.wstring()) + ": " + ec.message());
        const auto rel = it->path().lexically_relative(root);
        if (skip_anchor && it.depth() == 0 && is_anchor_file(rel)) continue;
        if (is_link(it->path())) return make_error("unsafe", "the data root contains a link: " + narrow(rel.wstring()));
        const auto st = it->symlink_status(ec);
        if (ec) return make_error("data_root", "cannot read " + narrow(rel.wstring()) + ": " + ec.message());
        if (fs::is_directory(st)) out.push_back({rel, true});
        else if (fs::is_regular_file(st)) out.push_back({rel, false});
        else return make_error("unsafe", "the data root contains an unsupported entry: " + narrow(rel.wstring()));
    }
    if (ec) return make_error("data_root", "cannot read " + narrow(root.wstring()) + ": " + ec.message());
    std::sort(out.begin(), out.end());
    return out;
}

bool same_bytes(const fs::path& a, const fs::path& b) {
    std::ifstream fa(a, std::ios::binary), fb(b, std::ios::binary);
    if (!fa || !fb) return false;
    std::vector<char> ba(1 << 16), bb(1 << 16);
    for (;;) {
        fa.read(ba.data(), static_cast<std::streamsize>(ba.size()));
        fb.read(bb.data(), static_cast<std::streamsize>(bb.size()));
        const auto na = fa.gcount(), nb = fb.gcount();
        if (na != nb || !std::equal(ba.begin(), ba.begin() + na, bb.begin())) return false;
        if (na == 0) return fa.eof() && fb.eof();
    }
}

// Removes a folder's content (keeping the anchor's record when it is the anchor) and then the folder itself unless
// it is the anchor. A missing folder is already removed.
Status remove_root(const fs::path& dir, const fs::path& anchor) {
    if (!exists_no_follow(dir)) return {};
    const bool is_anchor = same_path(dir, anchor);
    std::error_code ec;
    if (!is_anchor) {
        fs::remove_all(dir, ec);
        if (ec) return make_error("data_root", "cannot delete " + narrow(dir.wstring()) + ": " + ec.message());
        return {};
    }
    std::vector<fs::path> victims;
    for (fs::directory_iterator it(dir, ec); !ec && it != fs::directory_iterator(); it.increment(ec))
        if (!is_anchor_file(it->path())) victims.push_back(it->path());
    if (ec) return make_error("data_root", "cannot read " + narrow(dir.wstring()) + ": " + ec.message());
    for (const auto& v : victims) {
        fs::remove_all(v, ec);
        if (ec) return make_error("data_root", "cannot delete " + narrow(v.wstring()) + ": " + ec.message());
    }
    return {};
}

// Refusals keep the current root (client/local-storage Unsafe or occupied destination).
Status check_destination(const RootLocations& at, const fs::path& from, const fs::path& to, const fs::path& staging) {
    if (!to.is_absolute()) return make_error("refused", "the requested data root is not an absolute path");
    if (!strictly_inside(to, at.profile)) return make_error("refused", "the requested data root is outside the user profile");
    if (strictly_inside(to, from) || strictly_inside(from, to) || strictly_inside(staging, from) || same_path(staging, from))
        return make_error("refused", "the requested data root overlaps the current one");
    for (auto p = to; strictly_inside(p, at.profile); p = p.parent_path())
        if (is_link(p)) return make_error("refused", "the requested data root is reached through a link");
    if (exists_no_follow(staging)) return make_error("refused", "a folder named like the transition staging folder already exists");
    if (!exists_no_follow(to)) return {};
    std::error_code ec;
    if (!fs::is_directory(fs::symlink_status(to, ec))) return make_error("refused", "the requested data root is not a folder");
    const bool anchor = same_path(to, at.anchor);
    for (fs::directory_iterator it(to, ec); !ec && it != fs::directory_iterator(); it.increment(ec))
        if (!(anchor && is_anchor_file(it->path()))) return make_error("refused", "the requested data root already contains data");
    if (ec) return make_error("refused", "the requested data root cannot be read: " + ec.message());
    return {};
}

std::optional<fs::path> as_recorded(const fs::path& p, const RootLocations& at) {
    if (same_path(p, at.anchor)) return std::nullopt;
    return p;
}

Error interrupted() { return Error{"interrupted", "the root transition stopped (test)"}; }

// Before the commit point the source stays complete and unchanged. A move that cannot finish is undone, so the
// source can be used (and change) without a later resume installing the stale staged copy: entries already moved
// return to staging, staging goes, and the record is cleared. Only names that came from the source are moved back;
// whatever else is in the destination is left alone.
Status roll_back(const RootLocations& at, Boot& boot) {
    const auto& t = *boot.transition;
    std::error_code ec;
    if (t.stage == "moving") {
        const bool from_anchor = same_path(t.from, at.anchor);
        for (fs::directory_iterator it(t.from, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
            const auto name = it->path().filename();
            if (from_anchor && is_anchor_file(name)) continue;
            if (exists_no_follow(t.to / name) && !exists_no_follow(t.staging / name) &&
                !::MoveFileExW((t.to / name).c_str(), (t.staging / name).c_str(), MOVEFILE_WRITE_THROUGH))
                return make_error("data_root", "cannot undo the data-root move of " + name.string() + ": " + error_text(::GetLastError()));
        }
        if (ec) return make_error("data_root", "cannot read the current data root: " + ec.message());
    }
    fs::remove_all(t.staging, ec);
    if (ec) return make_error("data_root", "cannot delete the transition staging folder: " + ec.message());
    boot.transition.reset();
    return write_boot(at.anchor, boot);
}

// Completes a recorded transition from its stage. Before the commit point a failure keeps the source active.
Status resume(const RootLocations& at, Boot& boot, const TransitionStop& stop) {
    auto& t = *boot.transition;
    std::error_code ec;
    if (t.stage == "copying") {
        // The source is unchanged: drop the unverified copy; the caller starts again.
        fs::remove_all(t.staging, ec);
        if (ec) return make_error("data_root", "cannot delete the transition staging folder: " + ec.message());
        boot.transition.reset();
        return write_boot(at.anchor, boot);
    }
    if (t.stage == "moving") {
        // Staging was verified before this stage: finish moving it into the destination.
        std::string err;
        if (!create_private_directory(t.to.wstring(), err)) return make_error("data_root", err);
        if (exists_no_follow(t.staging)) {
            std::vector<fs::path> names;
            for (fs::directory_iterator it(t.staging, ec); !ec && it != fs::directory_iterator(); it.increment(ec))
                names.push_back(it->path().filename());
            if (ec) return make_error("data_root", "cannot read the transition staging folder: " + ec.message());
            for (const auto& n : names) {
                if (!::MoveFileExW((t.staging / n).c_str(), (t.to / n).c_str(), MOVEFILE_WRITE_THROUGH))
                    return make_error("data_root", "cannot move " + n.string() + " into the new data root: " + error_text(::GetLastError()));
                if (stop && stop(TransitionPoint::moved_one)) return interrupted();
            }
            fs::remove(t.staging, ec);
            if (ec) return make_error("data_root", "cannot delete the transition staging folder: " + ec.message());
        }
        t.stage = "switched";
        boot.active = as_recorded(t.to, at);
        if (auto s = write_boot(at.anchor, boot); !s) return s;
        if (stop && stop(TransitionPoint::switched)) return interrupted();
    }
    // switched: the destination is active; the previous root and staging go.
    if (auto s = remove_root(t.from, at.anchor); !s) return s;
    fs::remove_all(t.staging, ec);
    if (ec) return make_error("data_root", "cannot delete the transition staging folder: " + ec.message());
    boot.transition.reset();
    return write_boot(at.anchor, boot);
}

Status stage_copy(const fs::path& from, const fs::path& staging, const std::vector<Entry>& source);

// Starts a transition from the active root to `to`. Refusals and failures before the commit point keep `from`.
Status transition(const RootLocations& at, Boot& boot, const fs::path& from, const fs::path& to, const TransitionStop& stop) {
    const fs::path staging = to.parent_path() / (to.filename().wstring() + kStagingSuffix);
    if (auto s = check_destination(at, from, to, staging); !s) return s;
    if (!exists_no_follow(from)) {  // nothing to carry over yet
        boot.active = as_recorded(to, at);
        return write_boot(at.anchor, boot);
    }
    const bool from_anchor = same_path(from, at.anchor);
    auto source = list_tree(from, from_anchor);
    if (!source) return make_error("refused", source.error().message);

    boot.transition = Transition{from, to, staging, "copying"};
    if (auto s = write_boot(at.anchor, boot); !s) return s;
    if (auto s = stage_copy(from, staging, *source); !s) {
        (void)roll_back(at, boot);
        return s;
    }
    if (stop && stop(TransitionPoint::staged)) return interrupted();

    boot.transition->stage = "moving";
    if (auto s = write_boot(at.anchor, boot); !s) return s;
    return resume(at, boot, stop);
}

// Copies the source into a new private staging folder and verifies every path and byte against the source.
Status stage_copy(const fs::path& from, const fs::path& staging, const std::vector<Entry>& source) {
    std::error_code ec;
    fs::create_directories(staging.parent_path(), ec);
    std::string err;
    if (ec || !create_private_directory(staging.wstring(), err))
        return make_error("data_root", "cannot create the transition staging folder: " + (ec ? ec.message() : err));
    for (const auto& e : source) {
        if (e.dir) fs::create_directories(staging / e.rel, ec);
        else fs::copy_file(from / e.rel, staging / e.rel, fs::copy_options::none, ec);
        if (ec) return make_error("data_root", "cannot copy " + narrow(e.rel.wstring()) + ": " + ec.message());
    }
    // Verify the staged profile against the unchanged source before anything relies on it.
    auto staged = list_tree(staging, false);
    if (!staged) return staged.error();
    if (*staged != source) return make_error("data_root", "the staged data root does not match the current one");
    for (const auto& e : source)
        if (!e.dir && !same_bytes(from / e.rel, staging / e.rel))
            return make_error("data_root", "the staged copy of " + narrow(e.rel.wstring()) + " differs");
    return {};
}

}  // namespace

fs::path DataRoot::default_root() { return fs::path(local_appdata()) / L"Skowork"; }
RootLocations DataRoot::default_locations() { return {default_root(), profile_dir()}; }

bool inside_user_profile(const fs::path& p) { return strictly_inside(p, profile_dir()); }

Result<DataRoot> DataRoot::open(const std::optional<fs::path>& override_root) {
    if (!override_root) return open_managed(default_locations());
    DataRoot r;
    r.root_ = *override_root;
    std::string err;
    if (!create_private_directory(r.root_.wstring(), err)) return make_error("data_root", err);

    const std::string sid = narrow(current_user_sid());
    const auto binding_path = r.root_ / kBindingFile;
    if (auto binding = read_json_file(binding_path)) {
        const std::string bound = jstr(*binding, "windows_sid");
        if (!bound.empty() && bound != sid)
            return make_error("other_user", "the data root belongs to another Windows user (" + bound + ")");
    } else {
        json b = {{"windows_sid", sid}, {"schema", 1}};
        if (!write_json_file(binding_path, b)) return make_error("data_root", "cannot write binding file");
    }
    WithdrawalOutbox outbox(r.file("withdrawals.dpapi"));
    if (auto s = outbox.finish_interrupted_wipe([&] { return r.wipe(); }); !s) return s.error();
    std::error_code ec;
    fs::create_directories(r.logs(), ec);
    fs::create_directories(r.cache(), ec);
    return r;
}

Result<DataRoot> DataRoot::open_managed(const RootLocations& at, const TransitionStop& stop) {
    std::string note;
    fs::path active;
    {
        AnchorLock lock(at.anchor);
        if (!lock) return make_error("data_root", "another Skowork process holds the data root");
        auto boot = read_boot(at.anchor);
        if (!boot) return boot.error();
        // A step that fails is judged by the durable record: after the commit point the destination is active and
        // cleanup is retried at the next start; before it the move is undone so the source can be used.
        const auto settle = [&](const Error& failed) -> Status {
            if (failed.code == "interrupted") return failed;
            note = failed.message;
            boot = read_boot(at.anchor);
            if (!boot) return boot.error();
            if (boot->transition && boot->transition->stage != "switched")
                if (auto s = roll_back(at, *boot); !s) return s;
            return {};
        };
        bool attempted = false;
        if (boot->transition) {
            attempted = true;
            if (auto s = resume(at, *boot, stop); !s)
                if (auto settled = settle(s.error()); !settled) return settled.error();
        }
        const fs::path requested = boot->requested.value_or(at.anchor);
        // One attempt per start: a transition that just failed is not repeated until the next start.
        if (!(attempted && !note.empty()) && !boot->transition && !same_path(requested, boot->active.value_or(at.anchor))) {
            if (auto s = transition(at, *boot, boot->active.value_or(at.anchor), requested, stop); !s)
                if (auto settled = settle(s.error()); !settled) return settled.error();
        }
        active = boot->active.value_or(at.anchor);
    }
    auto r = open(active);
    if (!r) return r;
    r->managed_ = at;
    r->transition_note_ = note;
    return r;
}

Status DataRoot::set_policy_root(const std::optional<fs::path>& root) { return set_requested_root(default_locations(), root); }

Status DataRoot::set_requested_root(const RootLocations& at, const std::optional<fs::path>& root) {
    if (root && (!root->is_absolute() || !strictly_inside(*root, at.profile)))
        return make_error("policy", "policy.storage.root must be an absolute folder inside the user profile");
    AnchorLock lock(at.anchor);
    if (!lock) return make_error("data_root", "another Skowork process holds the data root");
    auto boot = read_boot(at.anchor);
    if (!boot) return boot.error();
    const auto wanted = root ? as_recorded(*root, at) : std::nullopt;
    const bool unchanged = wanted ? (boot->requested && same_path(*boot->requested, *wanted)) : !boot->requested;
    if (unchanged) return {};
    boot->requested = wanted;
    return write_boot(at.anchor, *boot);
}

Status DataRoot::wipe() const {
    WithdrawalOutbox outbox(file("withdrawals.dpapi"));
    if (auto state = outbox.load(); !state) return state.error();
    std::error_code ec;
    if (managed_) {
        // Neither a staging copy nor the inactive side of a recorded transition may outlive Forget Data.
        AnchorLock lock(managed_->anchor);
        if (!lock) return make_error("wipe", "another Skowork process holds the data root");
        auto boot = read_boot(managed_->anchor);
        if (!boot) return boot.error();
        if (boot->transition) {
            const auto& t = *boot->transition;
            fs::remove_all(t.staging, ec);
            if (ec) return make_error("wipe", "cannot delete the transition staging folder: " + ec.message());
            for (const auto& side : {t.from, t.to})
                if (!same_path(side, root_))
                    if (auto s = remove_root(side, managed_->anchor); !s) return make_error("wipe", s.error().message);
            boot->transition.reset();
            if (auto s = write_boot(managed_->anchor, *boot); !s) return s;
        }
    }
    fs::directory_iterator it(root_, ec);
    if (ec) return make_error("wipe", "cannot enumerate the data root: " + ec.message());
    std::vector<fs::path> to_remove;
    for (; it != fs::directory_iterator(); it.increment(ec)) {
        if (ec) return make_error("wipe", "cannot enumerate the data root: " + ec.message());
        const auto name = it->path().filename().string();
        if (name == kBindingFile || is_anchor_file(it->path()) || name == "withdrawals.dpapi") continue;
        to_remove.push_back(it->path());
    }
    if (ec) return make_error("wipe", "cannot enumerate the data root: " + ec.message());
    for (const auto& path : to_remove) {
        fs::remove_all(path, ec);
        if (ec) return make_error("wipe", "cannot delete " + path.filename().string() + ": " + ec.message());
    }
    for (fs::directory_iterator check(root_, ec); !ec && check != fs::directory_iterator(); check.increment(ec)) {
        const auto name = check->path().filename().string();
        if (name != kBindingFile && !is_anchor_file(check->path()) && name != "withdrawals.dpapi")
            return make_error("wipe", "data changed while it was being deleted; another attempt is needed");
    }
    if (ec) return make_error("wipe", "cannot verify the data root: " + ec.message());
    fs::create_directories(logs(), ec);
    if (ec) return make_error("wipe", "cannot recreate the log directory: " + ec.message());
    fs::create_directories(cache(), ec);
    if (ec) return make_error("wipe", "cannot recreate the cache directory: " + ec.message());
    return {};
}

}  // namespace wb
