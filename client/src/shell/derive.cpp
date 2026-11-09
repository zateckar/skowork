#include "shell/derive.h"

namespace wb::shell {

namespace {

const json& sub(const json& j, const char* key) {
    static const json empty = json::object();
    const json* o = jobj(j, key);
    return o && o->is_object() ? *o : empty;
}

std::string auth_state(const json& s) { return jstr(sub(s, "auth"), "state"); }

}  // namespace

McpHealth mcp_health(const json& status) {
    McpHealth h;
    const json* list = jobj(status, "mcp");
    if (!list || !list->is_array()) return h;
    size_t bad = 0;
    for (const json& s : *list) {
        ++h.total;
        const std::string state = jstr(s, "state");
        const std::string name = jstr(s, "name", jstr(s, "id"));
        if (state == "failed") {
            h.failed.push_back(name);
            ++bad;
        } else {
            h.usable_tools += jint(s, "usable_tools");
            if (state == "unavailable") {
                h.unavailable.push_back(name);
                ++bad;
            } else if (!jstr(s, "warning").empty()) {
                h.warnings.push_back(name);
                ++bad;
            }
        }
    }
    h.healthy = h.total - bad;
    h.tone = h.total == 0                                         ? ""
             : !h.failed.empty()                                 ? "bad"
             : !h.unavailable.empty() || !h.warnings.empty()     ? "warn"
                                                                 : "ok";
    return h;
}

bool is_not_configured(const json& s) {
    return !jbool(sub(s, "control_plane"), "configured") || auth_state(s) == "unconfigured";
}

std::string connection_kind(const json& s) {
    const json& cp = sub(s, "control_plane");
    if (is_not_configured(s)) return "not_configured";
    if (jbool(cp, "locked")) return "locked";
    if (jbool(cp, "version_blocked")) return "update_required";
    const std::string auth = auth_state(s);
    if (auth == "starting" || auth == "signing_in") return "connecting";
    if (auth == "sign_in_required") return "disconnected";
    return jbool(cp, "connected") ? "connected" : "disconnected";
}

std::vector<BannerSpec> banners(const json& s) {
    std::vector<BannerSpec> out;
    const json& cp = sub(s, "control_plane");
    const json& storage = sub(s, "storage");
    if (jint(storage, "pending_remote_removals") > 0) out.push_back({"remote_removal_pending", "warning"});
    if (jbool(cp, "locked")) out.push_back({"locked", "error"});
    if (jbool(cp, "version_blocked")) out.push_back({"version_blocked", "error"});
    const std::string auth = auth_state(s);
    if (is_not_configured(s))
        out.push_back({"not_configured", "warning"});
    else if (auth == "sign_in_required")
        out.push_back({"sign_in_required", "warning"});
    else if (auth == "signing_in")
        out.push_back({"signing_in", "info"});
    // Degraded mode implies read-only; one notice is enough.
    if (jbool(storage, "degraded"))
        out.push_back({"storage_degraded", "warning"});
    else if (jbool(storage, "read_only"))
        out.push_back({"storage_read_only", "warning"});
    return out;
}

std::vector<std::string> deferral_reasons(const json& s) {
    const json& b = sub(s, "background");
    std::vector<std::string> out;
    for (const char* k : {"battery_saver", "presentation", "indexing_hold", "storage_indexing_paused"})
        if (jbool(b, k)) out.emplace_back(k);
    return out;
}

bool is_app_status(const json& s) {
    if (!s.is_object()) return false;
    for (const char* k : {"version", "auth", "control_plane", "background", "storage"}) {
        const json* o = jobj(s, k);
        if (!o || !o->is_object()) return false;
    }
    return true;
}

}  // namespace wb::shell
