#include "storage/data_root.h"

#include "base/json.h"
#include "base/strings.h"
#include "platform/win.h"

#include <shlobj.h>

#include <fstream>
#include <sstream>

namespace wb {
namespace {

constexpr char kBindingFile[] = "binding.json";
constexpr char kBootstrapFile[] = "bootstrap.json";

std::optional<json> read_json_file(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return std::nullopt;
    std::stringstream ss;
    ss << f.rdbuf();
    return parse_json(ss.str());
}

bool write_json_file(const std::filesystem::path& p, const json& j) {
    auto tmp = p;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f << j.dump(2);
        if (!f) return false;
    }
    std::error_code ec;
    std::filesystem::rename(tmp, p, ec);
    return !ec;
}

std::filesystem::path profile_dir() {
    wchar_t* p = nullptr;
    std::filesystem::path out;
    if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_Profile, KF_FLAG_DEFAULT, nullptr, &p))) out = p;
    ::CoTaskMemFree(p);
    return out;
}

}  // namespace

std::filesystem::path DataRoot::default_root() { return std::filesystem::path(local_appdata()) / L"Workbench"; }

bool inside_user_profile(const std::filesystem::path& p) {
    const auto profile = to_lower(narrow(profile_dir().lexically_normal().wstring()));
    const auto target = to_lower(narrow(std::filesystem::absolute(p).lexically_normal().wstring()));
    return !profile.empty() && starts_with(target, profile + "\\");
}

Result<DataRoot> DataRoot::open(const std::optional<std::filesystem::path>& override_root) {
    DataRoot r;
    if (override_root) {
        r.root_ = *override_root;
    } else {
        r.root_ = default_root();
        if (auto boot = read_json_file(default_root() / kBootstrapFile)) {
            const std::string pr = jstr(*boot, "data_root");
            if (!pr.empty() && inside_user_profile(widen(pr))) r.root_ = widen(pr);
        }
    }
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
    std::error_code ec;
    std::filesystem::create_directories(r.logs(), ec);
    std::filesystem::create_directories(r.cache(), ec);
    return r;
}

Status DataRoot::set_policy_root(const std::optional<std::filesystem::path>& root) {
    std::string err;
    if (!create_private_directory(default_root().wstring(), err)) return make_error("data_root", err);
    const auto boot = default_root() / kBootstrapFile;
    if (!root) {
        std::error_code ec;
        std::filesystem::remove(boot, ec);
        return {};
    }
    if (!inside_user_profile(*root)) return make_error("policy", "policy.storage.root must be inside the user profile");
    if (!write_json_file(boot, json{{"data_root", narrow(root->wstring())}}))
        return make_error("data_root", "cannot write bootstrap file");
    return {};
}

Status DataRoot::wipe() const {
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(root_, ec)) {
        const auto name = e.path().filename().string();
        if (name == kBindingFile || name == kBootstrapFile) continue;
        std::filesystem::remove_all(e.path(), ec);
        if (ec) return make_error("wipe", "cannot delete " + name + ": " + ec.message());
    }
    std::filesystem::create_directories(logs(), ec);
    std::filesystem::create_directories(cache(), ec);
    return {};
}

}  // namespace wb
