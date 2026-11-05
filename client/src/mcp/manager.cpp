#include "mcp/manager.h"

#include "base/encoding.h"
#include "base/strings.h"
#include "base/time.h"
#include "platform/crypto.h"
#include "platform/log.h"
#include "platform/win.h"
#include "policy/decide.h"

#include <miniz.h>

#include <algorithm>
#include <fstream>
#include <sstream>

namespace wb::mcp {

namespace fs = std::filesystem;
using runtime::Provenance;
using runtime::ToolContext;
using runtime::ToolDef;
using runtime::ToolOutput;

namespace {

constexpr const char* kLog = "mcp";
constexpr auto kCrashWindow = std::chrono::hours(1);
constexpr size_t kMaxCrashes = 3;  // restarts per hour; the next crash marks the server failed
constexpr auto kRetryUnavailable = std::chrono::minutes(5);
constexpr size_t kMaxPackageFiles = 10000;
constexpr unsigned long long kMaxPackageBytes = 1024ull * 1024 * 1024;

std::string tool_id(const std::string& server, const std::string& tool) { return "mcp:" + server + "/" + tool; }

// Splits mcp:<server>/<tool>.
bool split_tool_id(const std::string& id, std::string& server, std::string& tool) {
    if (!starts_with(id, "mcp:")) return false;
    const size_t slash = id.find('/', 4);
    if (slash == std::string::npos) return false;
    server = id.substr(4, slash - 4);
    tool = id.substr(slash + 1);
    return true;
}

// One argument of a Windows command line (CommandLineToArgvW rules).
std::wstring quote_arg(const std::wstring& a) {
    if (!a.empty() && a.find_first_of(L" \t\n\v\"") == std::wstring::npos) return a;
    std::wstring q = L"\"";
    size_t backslashes = 0;
    for (const wchar_t c : a) {
        if (c == L'\\') {
            ++backslashes;
            continue;
        }
        if (c == L'"') q.append(backslashes * 2 + 1, L'\\');
        else q.append(backslashes, L'\\');
        backslashes = 0;
        q.push_back(c);
    }
    q.append(backslashes * 2, L'\\');
    q.push_back(L'"');
    return q;
}

std::optional<std::string> read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return std::nullopt;
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

bool write_file_atomic(const fs::path& p, std::string_view data) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    const fs::path tmp = p.wstring() + L".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(data.data(), static_cast<std::streamsize>(data.size()));
        if (!f) return false;
    }
    fs::rename(tmp, p, ec);
    if (ec) fs::remove(tmp, ec);
    return !ec;
}

std::string sha256_hex(std::string_view data) { return hex_encode(sha256(data)); }

// Extracts the package zip into `dir` (emptied first). Entry names must be safe relative paths.
Status extract_package(const std::string& bytes, const fs::path& dir) {
    std::error_code ec;
    fs::remove_all(dir, ec);
    if (ec) return make_error("package", "cannot clear " + narrow(dir.wstring()) + ": " + ec.message());
    fs::create_directories(dir, ec);
    if (ec) return make_error("package", "cannot create " + narrow(dir.wstring()));
    mz_zip_archive zip{};
    if (!mz_zip_reader_init_mem(&zip, bytes.data(), bytes.size(), 0)) return make_error("package", "the package is not a zip archive");
    struct Closer {
        mz_zip_archive* z;
        ~Closer() { mz_zip_reader_end(z); }
    } closer{&zip};
    const mz_uint n = mz_zip_reader_get_num_files(&zip);
    if (n > kMaxPackageFiles) return make_error("package", "the package has too many files");
    unsigned long long total = 0;
    for (mz_uint i = 0; i < n; ++i) {
        mz_zip_archive_file_stat st{};
        if (!mz_zip_reader_file_stat(&zip, i, &st)) return make_error("package", "unreadable zip entry");
        const std::string name = st.m_filename;
        if (!safe_relative_path(name.back() == '/' ? name.substr(0, name.size() - 1) : name))
            return make_error("package", "unsafe path in the package: " + name);
        const fs::path target = dir / fs::path(widen(name)).make_preferred();
        if (st.m_is_directory) {
            fs::create_directories(target, ec);
            continue;
        }
        total += st.m_uncomp_size;
        if (total > kMaxPackageBytes) return make_error("package", "the package expands beyond 1 GB");
        size_t size = 0;
        void* data = mz_zip_reader_extract_to_heap(&zip, i, &size, 0);
        if (!data) return make_error("package", "cannot extract " + name);
        fs::create_directories(target.parent_path(), ec);
        std::ofstream f(target, std::ios::binary | std::ios::trunc);
        f.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
        mz_free(data);
        if (!f) return make_error("package", "cannot write " + name);
    }
    return {};
}

}  // namespace

struct McpManager::Server {
    ServerEntry entry;
    // Under McpManager::mx_.
    std::string state = "idle";  // connected | idle | starting | failed | unavailable
    std::string error, warning;
    std::vector<ToolInfo> tools;
    bool tools_known = false;
    size_t usable = 0;
    bool removed = false;
    int in_flight = 0;
    std::chrono::steady_clock::time_point last_used{}, last_attempt{};
    std::deque<std::chrono::steady_clock::time_point> crashes;
    // Under call_mx.
    std::mutex call_mx;
    std::shared_ptr<Session> session;
    // Under reg_mx (never held together with McpManager::mx_ while calling into the runtime).
    std::mutex reg_mx;
    std::vector<std::string> registered;
};

McpManager::McpManager(McpEnv env) : env_(std::move(env)) {
    worker_ = std::thread([this] { worker(); });
}

McpManager::~McpManager() { shutdown(); }

// ---- Background work ------------------------------------------------------------------------------------------------

void McpManager::post(std::function<void()> fn) {
    {
        const std::lock_guard lock(work_mx_);
        if (stopping_) return;
        work_.push_back(std::move(fn));
    }
    work_cv_.notify_all();
}

void McpManager::worker() {
    for (;;) {
        std::function<void()> fn;
        {
            std::unique_lock lock(work_mx_);
            work_cv_.wait(lock, [&] { return stopping_ || !work_.empty(); });
            if (stopping_) return;
            fn = std::move(work_.front());
            work_.pop_front();
            ++busy_;
        }
        try {
            fn();
        } catch (const std::exception& e) {
            log::error(kLog, std::string("background task: ") + e.what());
        }
        {
            const std::lock_guard lock(work_mx_);
            --busy_;
        }
        work_cv_.notify_all();
    }
}

bool McpManager::wait_idle(std::chrono::milliseconds timeout) {
    std::unique_lock lock(work_mx_);
    return work_cv_.wait_for(lock, timeout, [&] { return work_.empty() && busy_ == 0; });
}

void McpManager::changed() {
    if (env_.on_status) env_.on_status();
}

// ---- Catalog --------------------------------------------------------------------------------------------------------

void McpManager::apply() {
    const auto p = env_.policy ? env_.policy() : nullptr;
    std::vector<std::string> problems;
    const std::vector<ServerEntry> entries = parse_entries(p ? p->find("mcp.servers") : nullptr, &problems);
    for (const auto& pr : problems) log::warn(kLog, "catalog entry skipped: " + pr);

    std::vector<ServerPtr> removed, added, kept;
    {
        const std::lock_guard lock(mx_);
        for (auto it = servers_.begin(); it != servers_.end();) {
            const auto e = std::find_if(entries.begin(), entries.end(), [&](const ServerEntry& x) { return x.id == it->first; });
            if (e == entries.end() || e->fingerprint != it->second->entry.fingerprint) {
                it->second->removed = true;
                removed.push_back(it->second);
                it = servers_.erase(it);
            } else {
                kept.push_back(it->second);
                ++it;
            }
        }
        for (const auto& e : entries) {
            if (servers_.count(e.id)) continue;
            auto s = std::make_shared<Server>();
            s->entry = e;
            servers_[e.id] = s;
            added.push_back(s);
        }
    }
    // Removed and changed servers: tools withdrawn and the server stopped before this returns.
    for (const auto& s : removed) {
        withdraw_tools(s);
        stop_session(s);
        log::info(kLog, "server " + s->entry.id + " removed from the catalog or changed");
    }
    // Unchanged servers: the policy may have changed which of their tools are usable.
    for (const auto& s : kept) {
        bool known = false, failed = false;
        std::vector<ToolInfo> tools;
        {
            const std::lock_guard lock(mx_);
            known = s->tools_known;
            failed = s->state == "unavailable";
            tools = s->tools;
        }
        if (known) {
            register_tools(s, tools);
        } else if (failed) {
            // Discovery failed before (for example refused by policy.network.egress_allow): the new policy may allow it.
            std::weak_ptr<Server> w = s;
            post([this, w] {
                if (auto sp = w.lock()) (void)discover(sp);
            });
        }
    }
    for (const auto& s : added) {
        // The cached tool list lets the tools register without starting the server.
        bool cached = false;
        if (auto text = read_file(env_.data_dir / ("tools-" + s->entry.id + ".json"))) {
            const auto j = parse_json(*text);
            if (j && jstr(*j, "fingerprint") == s->entry.fingerprint) {
                std::string cursor;
                if (auto tools = parse_tools_page(*j, cursor)) {
                    register_tools(s, *tools);
                    cached = true;
                }
            }
        }
        if (!cached) {
            {
                const std::lock_guard lock(mx_);
                s->state = "starting";
            }
            std::weak_ptr<Server> w = s;
            post([this, w] {
                if (auto sp = w.lock()) (void)discover(sp);
            });
        }
    }
    if (!removed.empty() || !added.empty() || !kept.empty()) changed();
}

Status McpManager::discover(const ServerPtr& s) {
    std::vector<ToolInfo> tools;
    {
        const std::lock_guard call(s->call_mx);
        {
            const std::lock_guard lock(mx_);
            if (s->removed) return make_error("removed");
            s->last_attempt = env_.now();
        }
        Status st = ensure_session(s);
        Result<std::vector<ToolInfo>> listed = st ? s->session->list_tools(env_.start_timeout) : Result<std::vector<ToolInfo>>(st.error());
        if (!listed) {
            {
                const std::lock_guard lock(mx_);
                s->state = s->state == "failed" ? "failed" : "unavailable";
                s->error = listed.error().message;
            }
            log::warn(kLog, "server " + s->entry.id + ": tools could not be listed: " + listed.error().message);
            changed();
            return listed.error();
        }
        tools = std::move(*listed);
    }
    json cache = {{"fingerprint", s->entry.fingerprint}, {"tools", json::array()}};
    for (const auto& t : tools) {
        json tj = {{"name", t.name}, {"description", t.description}, {"inputSchema", t.input_schema}};
        if (!t.title.empty()) tj["title"] = t.title;
        if (t.read_only_hint) tj["annotations"] = {{"readOnlyHint", true}};
        cache["tools"].push_back(std::move(tj));
    }
    if (!write_file_atomic(env_.data_dir / ("tools-" + s->entry.id + ".json"), cache.dump()))
        log::warn(kLog, "server " + s->entry.id + ": the tool list could not be cached");
    register_tools(s, tools);
    changed();
    return {};
}

// ---- Tools ----------------------------------------------------------------------------------------------------------

void McpManager::register_tools(const ServerPtr& s, const std::vector<ToolInfo>& tools) {
    const auto p = env_.policy ? env_.policy() : nullptr;
    const ServerEntry& e = s->entry;
    std::vector<ToolDef> defs;
    std::vector<std::string> reasons;
    for (const auto& t : tools) {
        const std::string id = tool_id(e.id, t.name);
        std::string why;
        if (!tool_selected(t.name, e.include_tools, e.exclude_tools, &why)) {
            reasons.push_back(t.name + " " + why);
            continue;
        }
        if (p) {
            const auto m = policy::most_specific(p->find("tools.rules"), id);
            if (m && m->decision == policy::Decision::deny) {
                reasons.push_back(t.name + " denied by policy (tools.rules[\"" + m->pattern + "\"], " + p->source("tools.rules[\"" + m->pattern + "\"]") + ")");
                continue;
            }
        }
        ToolDef d;
        d.id = id;
        d.description = "[" + e.name + "] " + (t.description.empty() ? t.title : t.description);
        d.parameters = t.input_schema;
        // The catalog's annotation, else external; the server's own annotations never lower it.
        const std::string declared = jstr(e.risk, t.name.c_str());
        d.risk = runtime::parse_risk(declared).value_or(runtime::Risk::external);
        d.provenance = Provenance{"mcp", e.id + "/" + t.name, "", false};  // shown as mcp:<server>/<tool>
        std::weak_ptr<Server> w = s;
        const std::string name = t.name;
        d.run = [this, w, name](ToolContext& ctx, const json& args) -> ToolOutput {
            auto sp = w.lock();
            if (!sp) return ToolOutput::failure("This MCP server is no longer in your catalog.");
            return call(sp, name, ctx, args);
        };
        defs.push_back(std::move(d));
    }
    std::string warning;
    if (defs.empty()) {
        if (tools.empty()) {
            warning = "no usable tools: the server offers no tools";
        } else {
            warning = "no usable tools: all " + std::to_string(tools.size()) + " tools are excluded by policy (";
            for (size_t i = 0; i < reasons.size() && i < 3; ++i) warning += (i ? "; " : "") + reasons[i];
            if (reasons.size() > 3) warning += "; ...";
            warning += ")";
        }
    }

    const std::lock_guard reg(s->reg_mx);
    {
        const std::lock_guard lock(mx_);
        if (s->removed) return;
        s->tools = tools;
        s->tools_known = true;
        if (s->state == "failed") defs.clear();  // a failed server's tools stay withdrawn
        s->usable = defs.size();
        s->warning = warning;
        if (s->state == "starting" || s->state == "unavailable") {
            s->state = "idle";
            s->error.clear();
        }
    }
    std::vector<std::string> ids;
    for (auto& d : defs) {
        ids.push_back(d.id);
        if (env_.add_tool)
            if (Status st = env_.add_tool(std::move(d)); !st) log::error(kLog, "tool registration: " + st.error().message);
    }
    for (const auto& old : s->registered)
        if (std::find(ids.begin(), ids.end(), old) == ids.end() && env_.remove_tool) env_.remove_tool(old);
    s->registered = std::move(ids);
}

void McpManager::withdraw_tools(const ServerPtr& s) {
    const std::lock_guard reg(s->reg_mx);
    for (const auto& id : s->registered)
        if (env_.remove_tool) env_.remove_tool(id);
    s->registered.clear();
    const std::lock_guard lock(mx_);
    s->usable = 0;
}

ToolOutput McpManager::call(const ServerPtr& s, const std::string& tool, ToolContext& ctx, const json& args) {
    const std::string id = tool_id(s->entry.id, tool);
    std::shared_ptr<Session> session;
    {
        const std::lock_guard call_lock(s->call_mx);
        {
            const std::lock_guard lock(mx_);
            if (s->removed) return ToolOutput::failure("The MCP server " + s->entry.id + " is no longer in your catalog.");
            if (s->state == "failed") return ToolOutput::failure("The MCP server " + s->entry.id + " has failed: " + s->error);
        }
        if (Status st = ensure_session(s); !st)
            return ToolOutput::failure("The MCP server " + s->entry.id + " could not be started: " + st.error().message);
        session = s->session;
        const std::lock_guard lock(mx_);
        ++s->in_flight;
        s->last_used = env_.now();
    }
    json arguments = args.is_object() ? args : json::object();
    arguments.erase("rationale");  // the runtime's approval rationale is not an argument of the MCP tool
    auto r = session->call_tool(tool, arguments, env_.call_timeout, [&ctx] { return ctx.cancelled(); });
    {
        const std::lock_guard lock(mx_);
        --s->in_flight;
        s->last_used = env_.now();
    }
    if (!r) {
        if (r.error().code == "closed") {
            if (auto* stdio = dynamic_cast<StdioTransport*>(&session->transport()); stdio && stdio->crashed()) {
                const std::string diag = stdio->diagnostics();
                bool counted = false;
                {
                    const std::lock_guard call_lock(s->call_mx);
                    if (s->session == session) {  // the first caller to see this crash counts it
                        s->session.reset();
                        counted = true;
                    }
                }
                stdio->close();
                if (counted) crashed(s, diag);
                return ToolOutput::failure("The MCP server " + s->entry.id + " stopped during the call (" + diag + ").");
            }
        }
        if (r.error().code == "cancelled") return ToolOutput::failure("The call was cancelled.");
        return ToolOutput::failure("The MCP call failed: " + r.error().message);
    }
    const CallText ct = call_result_text(*r);
    ToolOutput out;
    out.ok = !ct.is_error;
    out.content = ct.text;
    const std::string item = item_identifier(arguments, ct);
    const std::string source = s->entry.id + "/" + tool;  // kind:id reads mcp:<server>/<tool>#<item>
    out.provenance = Provenance{"mcp", item.empty() ? source : source + "#" + item, iso8601_utc_now(), false};
    return out;
}

// ---- Sessions -------------------------------------------------------------------------------------------------------

Status McpManager::ensure_session(const ServerPtr& s) {
    if (s->session && s->session->transport().alive()) return {};
    if (s->session) {
        auto* stdio = dynamic_cast<StdioTransport*>(&s->session->transport());
        const std::string diag = stdio ? stdio->diagnostics() : std::string{};
        const bool was_crash = stdio && stdio->crashed();
        s->session->transport().close();
        s->session.reset();
        if (was_crash) crashed(s, diag);
    }
    {
        const std::lock_guard lock(mx_);
        if (s->state == "failed") return make_error("failed", s->error);
    }
    auto t = s->entry.kind == "local" ? open_local(s) : open_remote(s);
    if (!t) {
        const std::lock_guard lock(mx_);
        if (s->state != "failed") s->state = "unavailable";
        s->error = t.error().message;
        return t.error();
    }
    auto session = Session::open(std::move(*t), s->entry.spec_version, env_.client_version, env_.start_timeout);
    if (!session) {
        const std::lock_guard lock(mx_);
        if (s->state != "failed") s->state = "unavailable";
        s->error = "the handshake failed: " + session.error().message;
        return make_error(session.error().code, s->error);
    }
    s->session = std::shared_ptr<Session>(std::move(*session));
    {
        const std::lock_guard lock(mx_);
        s->state = "connected";
        s->error.clear();
        s->last_used = env_.now();
    }
    log::info(kLog, "server " + s->entry.id + " connected");
    changed();
    return {};
}

void McpManager::stop_session(const ServerPtr& s) {
    std::shared_ptr<Session> session;
    {
        const std::lock_guard call(s->call_mx);
        session = std::move(s->session);
    }
    if (session) session->transport().close();
    const std::lock_guard lock(mx_);
    if (s->state == "connected") s->state = "idle";
}

void McpManager::crashed(const ServerPtr& s, const std::string& why) {
    bool failed = false;
    {
        const std::lock_guard lock(mx_);
        const auto now = env_.now();
        while (!s->crashes.empty() && now - s->crashes.front() > kCrashWindow) s->crashes.pop_front();
        s->crashes.push_back(now);
        if (s->crashes.size() > kMaxCrashes) {
            s->state = "failed";
            s->error = "crashed " + std::to_string(s->crashes.size()) + " times within an hour" + (why.empty() ? "" : " (" + why + ")");
            failed = true;
        } else {
            s->state = "idle";
            s->error = "the server crashed and is restarted on next use" + (why.empty() ? "" : " (" + why + ")");
        }
    }
    log::warn(kLog, "server " + s->entry.id + " crashed" + (failed ? "; marked failed" : "") + (why.empty() ? "" : ": " + why));
    if (failed) withdraw_tools(s);
    changed();
}

Result<std::unique_ptr<Transport>> McpManager::open_local(const ServerPtr& s) {
    const ServerEntry& e = s->entry;
    if (!env_.sandbox) return make_error("sandbox", "the tool sandbox is unavailable");
    auto bytes = verified_package(e);
    if (!bytes) return bytes.error();
    const fs::path run_dir = env_.data_dir / "run" / widen(e.id);
    if (Status st = extract_package(*bytes, run_dir); !st) return st.error();
    const fs::path exe = run_dir / fs::path(widen(e.args.front())).make_preferred();
    std::error_code ec;
    if (!fs::is_regular_file(exe, ec)) return make_error("package", "the package has no " + e.args.front());

    const auto p = env_.policy ? env_.policy() : nullptr;
    sandbox::ProcessSpec spec;
    spec.executable = exe.wstring();
    for (size_t i = 1; i < e.args.size(); ++i) spec.arguments += (i > 1 ? L" " : L"") + quote_arg(widen(e.args[i]));
    spec.working_dir = narrow(run_dir.wstring());
    spec.privileges = sandbox::Privileges::restricted;
    spec.network = sandbox::Network::proxy;
    spec.limits = sandbox::limits_for(p.get(), "mcp");
    spec.env = e.env;
    spec.tool = "mcp:" + e.id;
    auto proc = env_.sandbox->start(spec);
    if (!proc) return proc.error();
    return std::unique_ptr<Transport>(std::make_unique<StdioTransport>(std::move(*proc)));
}

Result<std::unique_ptr<Transport>> McpManager::open_remote(const ServerPtr& s) {
    const ServerEntry& e = s->entry;
    HttpTransportOptions o;
    o.url = e.url;
    o.client = env_.http ? env_.http : &http();
    o.guard = env_.guard;
    if (e.auth == "user-token") {
        o.token = [this](bool force) -> Result<std::string> {
            if (!env_.user_token) return make_error("unauthorized", "no access token");
            auto t = env_.user_token(force);
            if (!t) return make_error("unauthorized", "no access token for the MCP server: " + t.error().message);
            return t;
        };
    }
    return std::unique_ptr<Transport>(std::make_unique<HttpTransport>(std::move(o)));
}

Result<std::string> McpManager::verified_package(const ServerEntry& e) {
    const fs::path file = env_.data_dir / "packages" / (e.package + ".zip");
    // Checked on every start, and the bytes that were checked are the ones extracted.
    if (auto bytes = read_file(file); bytes && sha256_hex(*bytes) == e.package) return std::move(*bytes);
    std::error_code ec;
    fs::remove(file, ec);
    if (!env_.fetch_package) return make_error("package", "the package is not available");
    auto fetched = env_.fetch_package(e.package);
    if (!fetched) return make_error("package", "the package could not be fetched: " + fetched.error().message);
    const std::string actual = sha256_hex(*fetched);
    if (actual != e.package) {
        log::warn(kLog, "server " + e.id + ": the fetched package does not match the catalog hash");
        if (env_.security_event)
            env_.security_event("mcp_package_mismatch", {{"server", e.id}, {"expected", e.package}, {"actual", actual}});
        return make_error("package_mismatch", "the package does not match the catalog's SHA-256; the server is not started");
    }
    if (!write_file_atomic(file, *fetched)) log::warn(kLog, "server " + e.id + ": the package could not be stored");
    return std::move(*fetched);
}

// ---- Lifecycle ------------------------------------------------------------------------------------------------------

void McpManager::tick() {
    const auto p = env_.policy ? env_.policy() : nullptr;
    const auto idle = std::chrono::minutes(std::max<long long>(1, p ? p->integer("mcp.idle_timeout_minutes", 10) : 10));
    const auto now = env_.now();
    std::vector<ServerPtr> stop, retry;
    {
        const std::lock_guard lock(mx_);
        for (const auto& [id, s] : servers_) {
            (void)id;
            if (s->state == "connected" && s->in_flight == 0 && now - s->last_used >= idle) stop.push_back(s);
            if (s->state == "unavailable" && now - s->last_attempt >= kRetryUnavailable) retry.push_back(s);
        }
    }
    for (const auto& s : stop) {
        stop_session(s);
        log::info(kLog, "server " + s->entry.id + " stopped after being idle");
    }
    for (const auto& s : retry) {
        {
            const std::lock_guard lock(mx_);
            s->last_attempt = now;
        }
        std::weak_ptr<Server> w = s;
        post([this, w] {
            if (auto sp = w.lock()) (void)discover(sp);
        });
    }
    if (!stop.empty()) changed();
}

void McpManager::shutdown() {
    {
        const std::lock_guard lock(work_mx_);
        stopping_ = true;
        work_.clear();
    }
    work_cv_.notify_all();
    if (worker_.joinable()) worker_.join();
    std::map<std::string, ServerPtr> all;
    {
        const std::lock_guard lock(mx_);
        all.swap(servers_);
        for (auto& [id, s] : all) {
            (void)id;
            s->removed = true;
        }
    }
    for (auto& [id, s] : all) {
        (void)id;
        withdraw_tools(s);
        stop_session(s);
    }
}

// ---- Health ---------------------------------------------------------------------------------------------------------

json McpManager::status() const {
    json out = json::array();
    const std::lock_guard lock(mx_);
    for (const auto& [id, s] : servers_) {
        json j = {{"id", id},
                  {"name", s->entry.name},
                  {"kind", s->entry.kind},
                  {"state", s->state},
                  {"tools", s->tools_known ? json(s->tools.size()) : json(nullptr)},
                  {"usable_tools", s->usable}};
        j["warning"] = s->warning.empty() || s->state == "failed" ? json(nullptr) : json(s->warning);
        j["error"] = s->error.empty() ? json(nullptr) : json(s->error);
        out.push_back(std::move(j));
    }
    return out;
}

std::optional<std::string> McpManager::server_unavailable(const std::string& server_id) const {
    const std::lock_guard lock(mx_);
    const auto it = servers_.find(server_id);
    if (it == servers_.end()) return "the MCP server " + server_id + " is not in your catalog";
    if (it->second->state == "failed" || it->second->state == "unavailable")
        return "the MCP server " + server_id + " is " + it->second->state + ": " + it->second->error;
    return std::nullopt;
}

std::optional<std::string> McpManager::tool_unavailable(const std::string& id) const {
    std::string server, tool;
    if (!split_tool_id(id, server, tool)) return std::nullopt;
    const std::lock_guard lock(mx_);
    const auto it = servers_.find(server);
    if (it == servers_.end()) return "the MCP server " + server + " is not in your catalog";
    if (it->second->state == "failed") return "the MCP server " + server + " has failed: " + it->second->error;
    return std::nullopt;
}

}  // namespace wb::mcp
