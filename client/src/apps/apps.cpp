#include "apps/apps.h"

#include "base/strings.h"
#include "base/time.h"
#include "platform/crypto.h"
#include "platform/log.h"
#include "platform/win.h"
#include "policy/paths.h"
#include "sandbox/sandbox.h"
#include "base/encoding.h"

#include <algorithm>
#include <fstream>
#include <regex>
#include <sstream>

namespace wb::apps {

namespace fs = std::filesystem;

namespace {

constexpr const char* kLog = "apps";
constexpr const char* kPipe = "workbench-app";

std::optional<std::string> read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return std::nullopt;
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

bool write_file(const fs::path& p, std::string_view data) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(data.data(), static_cast<std::streamsize>(data.size()));
    return static_cast<bool>(f);
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

std::string final_path(const fs::path& p) {
    auto c = policy::canonicalize(narrow(p.wstring()));
    return c ? *c : narrow(p.wstring());
}

long long folder_size(const fs::path& p) {
    long long n = 0;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(p, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec))
        if (it->is_regular_file(ec)) n += static_cast<long long>(it->file_size(ec));
    return n;
}

bool valid_tool_id(const std::string& t) {
    static const std::regex re(R"(^mcp:[A-Za-z0-9_.-]+/[^/\s]+$)");
    return t.size() < 200 && std::regex_match(t, re);
}

// The starter files apps.create writes: a server for public/ and a small JSON API on data.db, and a page that uses it
// with relative URLs (the app is served under /<app-id>/).
constexpr const char* kStarterServer = R"JS(// A Workbench Local app: built-in Node.js modules only, no network, files in this folder only.
// The client serves this app to the user alone under /<app-id>/, so pages use relative URLs.
const http = require('node:http');
const fs = require('node:fs');
const path = require('node:path');
const { DatabaseSync } = require('node:sqlite');

const db = new DatabaseSync(path.join(__dirname, 'data.db'));
db.exec('CREATE TABLE IF NOT EXISTS notes (id INTEGER PRIMARY KEY, text TEXT NOT NULL)');

const types = { '.html': 'text/html; charset=utf-8', '.css': 'text/css; charset=utf-8', '.js': 'text/javascript; charset=utf-8',
  '.json': 'application/json', '.svg': 'image/svg+xml', '.png': 'image/png', '.jpg': 'image/jpeg', '.ico': 'image/x-icon' };
const publicDir = path.join(__dirname, 'public');

function sendJson(res, status, body) {
  res.writeHead(status, { 'content-type': 'application/json' });
  res.end(JSON.stringify(body));
}

http.createServer((req, res) => {
  const url = new URL(req.url, 'http://app');
  if (url.pathname === '/api/notes' && req.method === 'GET') return sendJson(res, 200, db.prepare('SELECT id, text FROM notes').all());
  if (url.pathname === '/api/notes' && req.method === 'POST') {
    let body = '';
    req.on('data', (c) => (body += c));
    req.on('end', () => {
      const { text } = JSON.parse(body || '{}');
      const r = db.prepare('INSERT INTO notes (text) VALUES (?)').run(String(text || ''));
      sendJson(res, 201, { id: Number(r.lastInsertRowid) });
    });
    return;
  }
  const file = path.join(publicDir, path.normalize(decodeURIComponent(url.pathname === '/' ? '/index.html' : url.pathname)));
  if (!file.startsWith(publicDir + path.sep)) return sendJson(res, 403, { error: 'forbidden' });
  fs.readFile(file, (err, data) => {
    if (err) return sendJson(res, 404, { error: 'not found' });
    res.writeHead(200, { 'content-type': types[path.extname(file)] || 'application/octet-stream' });
    res.end(data);
  });
}).listen(process.env.WORKBENCH_APP_PIPE);
)JS";

constexpr const char* kStarterPage = R"HTML(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Local app</title>
<style>
  body { font-family: "Segoe UI", sans-serif; margin: 2rem; color: #1b1b1b; }
</style>
</head>
<body>
<h1>Local app</h1>
<ul id="notes"></ul>
<script>
// Relative URLs: the app lives under /<app-id>/. Data of a confirmed MCP tool:
//   fetch('_workbench/mcp/<server>/<tool>', { method: 'POST', headers: { 'content-type': 'application/json' }, body: JSON.stringify(args) })
fetch('api/notes').then((r) => r.json()).then((notes) => {
  for (const n of notes) {
    const li = document.createElement('li');
    li.textContent = n.text;
    document.getElementById('notes').append(li);
  }
});
</script>
</body>
</html>
)HTML";

}  // namespace

// ---- Manifest -------------------------------------------------------------------------------------------------------

json Manifest::to_json() const {
    return {{"name", name}, {"description", description}, {"entry", entry}, {"mcp_tools", mcp_tools}};
}

Result<Manifest> parse_manifest(const json& j) {
    if (!j.is_object()) return make_error("manifest", "app.json must be a JSON object");
    Manifest m;
    m.name = trim(jstr(j, "name"));
    m.description = trim(jstr(j, "description"));
    m.entry = trim(jstr(j, "entry", "server.js"));
    if (m.name.empty() || m.name.size() > 100) return make_error("manifest", "app.json needs a name of at most 100 characters");
    if (m.description.size() > 2000) return make_error("manifest", "the description is longer than 2000 characters");
    std::string e = m.entry;
    for (auto& c : e)
        if (c == '/') c = '\\';
    const bool script = ends_with(to_lower(e), ".js") || ends_with(to_lower(e), ".mjs") || ends_with(to_lower(e), ".cjs");
    if (e.empty() || !script || e.find("..") != std::string::npos || e.find(':') != std::string::npos || e[0] == '\\')
        return make_error("manifest", "entry must be a .js, .mjs or .cjs file inside the app folder");
    m.entry = e;
    if (const json* t = jobj(j, "mcp_tools")) {
        if (!t->is_array()) return make_error("manifest", "mcp_tools must be a list of mcp:<server>/<tool> ids");
        for (const auto& x : *t) {
            if (!x.is_string() || !valid_tool_id(x.get<std::string>()))
                return make_error("manifest", "mcp_tools must be a list of mcp:<server>/<tool> ids");
            if (std::find(m.mcp_tools.begin(), m.mcp_tools.end(), x.get<std::string>()) == m.mcp_tools.end()) m.mcp_tools.push_back(x.get<std::string>());
        }
        if (m.mcp_tools.size() > 50) return make_error("manifest", "an app can name at most 50 MCP tools");
    }
    if (const json* d = jobj(j, "dependencies"); d && !d->empty())
        return make_error("dependencies", "Local apps cannot use packages; use built-in Node.js modules only");
    return m;
}

std::string slug(std::string_view name) {
    std::string out;
    for (const char c0 : fold_latin(name)) {
        const char c = static_cast<char>(c0 >= 'A' && c0 <= 'Z' ? c0 - 'A' + 'a' : c0);
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) out.push_back(c);
        else if (!out.empty() && out.back() != '-') out.push_back('-');
        if (out.size() >= 40) break;
    }
    while (!out.empty() && out.back() == '-') out.pop_back();
    return out.empty() ? "app" : out;
}

std::string tools_hash(const std::vector<std::string>& tools) {
    std::set<std::string> s(tools.begin(), tools.end());
    std::string joined;
    for (const auto& t : s) joined += t + "\n";
    return hex_encode(sha256(joined));
}

std::optional<std::string> dependency_problem(const fs::path& folder) {
    std::error_code ec;
    if (fs::exists(folder / "node_modules", ec))
        return "Local apps cannot use packages: remove the node_modules folder and use built-in Node.js modules only.";
    if (const auto pkg = read_file(folder / "package.json")) {
        const auto j = parse_json(*pkg);
        if (!j) return std::string("package.json is not valid JSON.");
        for (const char* k : {"dependencies", "devDependencies", "optionalDependencies", "peerDependencies", "bundledDependencies"})
            if (const json* d = jobj(*j, k); d && !d->empty())
                return "Local apps cannot use packages: package.json declares " + std::string(k) + ". Use built-in Node.js modules only.";
    }
    return std::nullopt;
}

// ---- Frames ---------------------------------------------------------------------------------------------------------

std::string encode_frame(std::string_view type, long long id, std::string_view body) {
    const std::string h = json{{"t", type}, {"id", id}}.dump();
    std::string out(8, '\0');
    const auto put = [&](size_t at, size_t v) {
        for (int i = 0; i < 4; ++i) out[at + static_cast<size_t>(i)] = static_cast<char>((v >> (8 * i)) & 0xFF);
    };
    put(0, h.size());
    put(4, body.size());
    out += h;
    out.append(body);
    return out;
}

std::optional<Frame> FrameReader::next() {
    if (bad_ || buf_.size() < 8) return std::nullopt;
    const auto get = [&](size_t at) {
        size_t v = 0;
        for (int i = 3; i >= 0; --i) v = (v << 8) | static_cast<unsigned char>(buf_[at + static_cast<size_t>(i)]);
        return v;
    };
    const size_t hl = get(0), bl = get(4);
    if (hl > 4096 || bl > (16u << 20)) {
        bad_ = true;
        return std::nullopt;
    }
    if (buf_.size() < 8 + hl + bl) return std::nullopt;
    const auto h = parse_json(std::string_view(buf_).substr(8, hl));
    if (!h || !h->is_object()) {
        bad_ = true;
        return std::nullopt;
    }
    Frame f;
    f.type = jstr(*h, "t");
    f.id = jint(*h, "id");
    f.body = buf_.substr(8 + hl, bl);
    buf_.erase(0, 8 + hl + bl);
    return f;
}

// ---- AppStream ------------------------------------------------------------------------------------------------------

Status AppStream::send(std::string_view bytes) {
    if (!write_) return make_error("stopped", "the app is not running");
    return write_("data", bytes);
}

void AppStream::end_input() {
    if (write_) (void)write_("end", {});
}

bool AppStream::read(std::string& out, std::chrono::milliseconds timeout) {
    std::unique_lock lock(mx_);
    if (!cv_.wait_for(lock, timeout, [this] { return !chunks_.empty() || ended_; })) {
        timed_out_ = true;
        return false;
    }
    if (chunks_.empty()) return false;
    out = std::move(chunks_.front());
    chunks_.pop_front();
    return true;
}

void AppStream::push(std::string bytes) {
    {
        const std::lock_guard lock(mx_);
        chunks_.push_back(std::move(bytes));
    }
    cv_.notify_all();
}

void AppStream::finish() {
    {
        const std::lock_guard lock(mx_);
        ended_ = true;
    }
    cv_.notify_all();
}

// ---- AppInfo --------------------------------------------------------------------------------------------------------

json AppInfo::to_json() const {
    json j = {{"id", id},       {"name", manifest.name}, {"description", manifest.description}, {"entry", manifest.entry},
              {"mcp_tools", manifest.mcp_tools}, {"folder", folder}, {"size", size}, {"running", running},
              {"needs_confirmation", needs_confirmation}};
    j["last_used"] = last_used ? iso8601_utc(sys_clock::time_point(std::chrono::milliseconds(last_used))) : std::string{};
    if (!problem.empty()) j["problem"] = problem;
    return j;
}

// ---- Running apps ---------------------------------------------------------------------------------------------------

struct AppManager::Running {
    std::string id;
    std::unique_ptr<sandbox::SandboxedProcess> proc;
    std::thread reader;
    std::mutex mx;
    std::condition_variable cv;
    std::map<long long, std::weak_ptr<AppStream>> streams;
    bool ready = false, ended = false;
    long long last_request = 0;

    ~Running() {
        if (proc) proc->terminate();
        if (reader.joinable()) reader.join();
    }
    Status write(std::string_view type, long long stream, std::string_view body) {
        if (!proc) return make_error("stopped", "the app is not running");
        return proc->write(encode_frame(type, stream, body));
    }
};

AppManager::AppManager(AppsEnv env) : env_(std::move(env)) {
    std::error_code ec;
    fs::create_directories(env_.root, ec);
    fs::create_directories(env_.state_dir / "logs", ec);
}

AppManager::~AppManager() { shutdown(); }

long long AppManager::now() const { return env_.now_ms ? env_.now_ms() : unix_millis_now(); }

void AppManager::notify() {
    if (env_.changed) env_.changed();
}

std::optional<std::string> AppManager::unavailable() const {
    const auto p = env_.policy ? env_.policy() : nullptr;
    if (p && p->string("apps.mode", "on") == "off") return std::string("Local apps are switched off by policy.apps.mode.");
    std::error_code ec;
    if (!fs::exists(env_.runtime_dir / "node.exe", ec) || !fs::exists(env_.runtime_dir / "app-host.js", ec))
        return std::string("The app runtime package is not installed.");
    if (!env_.sandbox) return std::string("The tool sandbox is not running.");
    return std::nullopt;
}

std::string AppManager::mcp_mode() const {
    const auto p = env_.policy ? env_.policy() : nullptr;
    return p ? p->string("apps.mcp", "read") : std::string("read");
}

fs::path AppManager::folder_of(const std::string& id) const { return env_.root / widen(id); }

json AppManager::state_locked() {
    const auto s = read_file(env_.state_dir / "apps-state.json");
    auto j = s ? parse_json(*s) : std::nullopt;
    return j && j->is_object() ? *j : json::object();
}

void AppManager::save_state_locked(const json& s) {
    const fs::path tmp = env_.state_dir / "apps-state.json.tmp";
    if (!write_file(tmp, s.dump(2))) return;
    std::error_code ec;
    fs::rename(tmp, env_.state_dir / "apps-state.json", ec);
}

Result<AppInfo> AppManager::info_locked(const std::string& id) {
    if (id.empty() || slug(id) != id) return make_error("not_found", "no app with the id " + id);
    const fs::path folder = folder_of(id);
    std::error_code ec;
    if (!fs::is_directory(folder, ec)) return make_error("not_found", "no app with the id " + id);
    AppInfo a;
    a.id = id;
    a.folder = final_path(folder);
    a.size = folder_size(folder);
    const json state = state_locked();
    if (const json* s = jobj(state, id.c_str())) a.last_used = jint(*s, "last_used");
    if (const auto r = running_.find(id); r != running_.end()) {
        a.running = !r->second->ended;
        a.last_used = std::max(a.last_used, r->second->last_request);
    }
    const auto text = read_file(folder / "app.json");
    const auto j = text ? parse_json(*text) : std::nullopt;
    if (!j) {
        a.manifest.name = id;
        a.problem = "app.json is missing or not valid JSON.";
        return a;
    }
    auto m = parse_manifest(*j);
    if (!m) {
        a.manifest.name = jstr(*j, "name", id);
        a.problem = m.error().message;
        return a;
    }
    a.manifest = *m;
    if (const auto d = dependency_problem(folder)) a.problem = *d;
    else if (std::error_code e2; !fs::is_regular_file(folder / widen(a.manifest.entry), e2))
        a.problem = "The entry file " + a.manifest.entry + " does not exist yet.";
    if (!a.manifest.mcp_tools.empty()) {
        const json* s = jobj(state, id.c_str());
        a.needs_confirmation = !s || jstr(*s, "confirmed") != tools_hash(a.manifest.mcp_tools);
    }
    return a;
}

std::vector<AppInfo> AppManager::list() {
    const std::lock_guard lock(mx_);
    reap_locked();
    std::vector<AppInfo> out;
    std::error_code ec;
    for (fs::directory_iterator it(env_.root, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_directory(ec)) continue;
        if (auto a = info_locked(narrow(it->path().filename().wstring()))) out.push_back(std::move(*a));
    }
    std::sort(out.begin(), out.end(), [](const AppInfo& x, const AppInfo& y) {
        return x.last_used != y.last_used ? x.last_used > y.last_used : x.id < y.id;
    });
    return out;
}

std::optional<AppInfo> AppManager::get(const std::string& id) {
    const std::lock_guard lock(mx_);
    reap_locked();
    auto a = info_locked(id);
    if (!a) return std::nullopt;
    return *a;
}

Result<AppInfo> AppManager::create(const Manifest& m0, const std::string& run_id) {
    auto m = parse_manifest(m0.to_json());
    if (!m) return m.error();
    std::string id;
    {
        const std::lock_guard lock(mx_);
        const std::string base = slug(m->name);
        std::error_code ec;
        id = base;
        for (int n = 2; fs::exists(folder_of(id), ec); ++n) id = base.substr(0, 36) + "-" + std::to_string(n);
        const fs::path folder = folder_of(id);
        if (!fs::create_directories(folder / "public", ec) || ec) return make_error("io", "cannot create the app folder: " + ec.message());
        if (!write_file(folder / "app.json", m->to_json().dump(2)) || !write_file(folder / widen(m->entry), kStarterServer) ||
            !write_file(folder / "public" / "index.html", kStarterPage))
            return make_error("io", "cannot write the app's files");
        if (!run_id.empty()) grants_[run_id].insert(id);
    }
    log::info(kLog, "created " + id);
    notify();
    const std::lock_guard lock(mx_);
    return info_locked(id);
}

Result<AppInfo> AppManager::update(const std::string& id, const json& changes, const std::string& run_id) {
    {
        const std::lock_guard lock(mx_);
        auto a = info_locked(id);
        if (!a) return a.error();
        json m = a->manifest.to_json();
        for (const char* k : {"name", "description", "entry", "mcp_tools"})
            if (const json* v = jobj(changes, k)) m[k] = *v;
        auto parsed = parse_manifest(m);
        if (!parsed) return parsed.error();
        if (!write_file(folder_of(id) / "app.json", parsed->to_json().dump(2))) return make_error("io", "cannot write app.json");
        if (!run_id.empty()) grants_[run_id].insert(id);
    }
    notify();
    const std::lock_guard lock(mx_);
    return info_locked(id);
}

std::vector<std::string> AppManager::folders_for(const std::string& run_id) {
    const std::lock_guard lock(mx_);
    std::set<std::string> ids;
    for (auto it = grants_.begin(); it != grants_.end();) {
        if (env_.run_active && !env_.run_active(it->first)) {
            it = grants_.erase(it);  // the Run ended: its access to the folders ends with it
            continue;
        }
        if (run_id.empty() || it->first == run_id) ids.insert(it->second.begin(), it->second.end());
        ++it;
    }
    std::vector<std::string> out;
    std::error_code ec;
    for (const auto& id : ids)
        if (fs::is_directory(folder_of(id), ec)) out.push_back(final_path(folder_of(id)));
    return out;
}

Status AppManager::confirm(const std::string& id) {
    {
        const std::lock_guard lock(mx_);
        auto a = info_locked(id);
        if (!a) return a.error();
        json s = state_locked();
        s[id]["confirmed"] = tools_hash(a->manifest.mcp_tools);
        save_state_locked(s);
    }
    notify();
    return {};
}

void AppManager::append_log(const std::string& id, std::string_view text) {
    const std::lock_guard lock(log_mx_);
    std::string& l = logs_[id];
    l.append(text);
    if (l.size() > env_.log_bytes) l.erase(0, l.size() - env_.log_bytes);
}

std::string AppManager::logs(const std::string& id, size_t max_bytes) {
    std::string l;
    {
        const std::lock_guard lock(log_mx_);
        if (const auto it = logs_.find(id); it != logs_.end()) l = it->second;
    }
    if (l.empty())
        if (const auto f = read_file(env_.state_dir / "logs" / widen(id + ".log"))) l = *f;
    if (l.size() > max_bytes) l.erase(0, l.size() - max_bytes);
    return l;
}

void AppManager::reap_locked() {
    for (auto it = running_.begin(); it != running_.end();) {
        if (it->second->ended) {
            const std::string id = it->first;
            it = running_.erase(it);
            const std::lock_guard lock(log_mx_);
            if (const auto l = logs_.find(id); l != logs_.end()) (void)write_file(env_.state_dir / "logs" / widen(id + ".log"), l->second);
        } else {
            ++it;
        }
    }
}

Status AppManager::start(const std::string& id) {
    std::unique_lock lock(mx_);
    return start_locked(lock, id);
}

Status AppManager::start_locked(std::unique_lock<std::mutex>& lock, const std::string& id) {
    reap_locked();
    if (const auto r = running_.find(id); r != running_.end()) return {};
    if (const auto why = unavailable()) return make_error("unavailable", *why);
    auto a = info_locked(id);
    if (!a) return a.error();
    if (!a->problem.empty()) return make_error(starts_with(a->problem, "Local apps cannot use packages") ? "dependencies" : "manifest", a->problem);
    if (a->needs_confirmation)
        return make_error("confirmation_needed", "The user has not confirmed the MCP tools this app uses.");
    // At most max_running apps: the least recently used one stops.
    while (running_.size() >= env_.max_running) {
        auto lru = std::min_element(running_.begin(), running_.end(),
                                    [](const auto& x, const auto& y) { return x.second->last_request < y.second->last_request; });
        const std::string oldest = lru->first;  // a copy: stop_locked erases the entry that holds the key
        stop_locked(oldest, "more than " + std::to_string(env_.max_running) + " apps were running");
    }

    const fs::path folder = folder_of(id);
    const std::wstring runtime = widen(final_path(env_.runtime_dir));
    const std::wstring app = widen(a->folder);
    sandbox::ProcessSpec spec;
    spec.executable = (env_.runtime_dir / "node.exe").wstring();
    spec.arguments = L"--permission " + quote_arg(L"--allow-fs-read=" + app) + L" " + quote_arg(L"--allow-fs-read=" + runtime) + L" " +
                     quote_arg(L"--allow-fs-write=" + app) + L" --require " + quote_arg(runtime + L"\\app-host.js") + L" " +
                     quote_arg(app + L"\\" + widen(a->manifest.entry));
    spec.working_dir = a->folder;
    spec.privileges = sandbox::Privileges::restricted;
    spec.network = sandbox::Network::none;
    spec.limits.memory_mb = env_.memory_mb;
    spec.limits.processes = 1;
    spec.limits.cpu_percent = 50;
    spec.env = {{"WORKBENCH_APP_PIPE", kPipe}, {"NODE_OPTIONS", ""}};
    spec.run_id = "app:" + id;
    spec.tool = "app:" + id;
    auto p = env_.sandbox->start(spec);
    if (!p) return make_error("start", "the app could not be started: " + p.error().message);

    auto r = std::make_shared<Running>();
    r->id = id;
    r->proc = std::move(*p);
    r->last_request = now();
    append_log(id, "[" + iso8601_utc_now() + "] started\n");
    Running* raw = r.get();
    r->reader = std::thread([this, raw] {
        FrameReader fr;
        for (;;) {
            const std::string chunk = raw->proc->read_some();
            if (chunk.empty()) break;
            fr.feed(chunk);
            while (auto f = fr.next()) {
                if (f->type == "log") {
                    append_log(raw->id, f->body);
                    continue;
                }
                std::shared_ptr<AppStream> s;
                {
                    const std::lock_guard l(raw->mx);
                    if (f->type == "ready") {
                        raw->ready = true;
                        raw->cv.notify_all();
                        continue;
                    }
                    if (const auto it = raw->streams.find(f->id); it != raw->streams.end()) s = it->second.lock();
                    if (f->type == "end") raw->streams.erase(f->id);
                }
                if (!s) continue;
                if (f->type == "data") s->push(std::move(f->body));
                else if (f->type == "end") s->finish();
            }
            if (fr.bad()) {
                append_log(raw->id, "[client] the app wrote malformed output; stopping it\n");
                raw->proc->terminate();
            }
        }
        raw->proc->wait_ended(std::chrono::seconds(2));
        const std::string tail = raw->proc->stderr_tail();
        if (!tail.empty()) append_log(raw->id, tail + (ends_with(tail, "\n") ? "" : "\n"));
        append_log(raw->id, "[" + iso8601_utc_now() + "] stopped (exit code " + std::to_string(raw->proc->exit_code()) +
                                (raw->proc->limit() != sandbox::LimitHit::none ? std::string(", ") + sandbox::to_string(raw->proc->limit()) + " limit" : "") +
                                ")\n");
        std::map<long long, std::weak_ptr<AppStream>> streams;
        {
            const std::lock_guard l(raw->mx);
            raw->ended = true;
            streams.swap(raw->streams);
            raw->cv.notify_all();
        }
        for (auto& [_, w] : streams)
            if (auto s = w.lock()) s->finish();
        notify();
    });

    bool ok = false;
    {
        std::unique_lock l(r->mx);
        ok = r->cv.wait_for(l, env_.start_timeout, [&] { return r->ready || r->ended; }) && r->ready;
    }
    if (!ok) {
        r.reset();  // terminates and joins
        const std::string tail = logs(id, 2000);
        return make_error("start", "The app did not start its server. It must call "
                                   "http.createServer(...).listen(process.env.WORKBENCH_APP_PIPE). Log:\n" + tail);
    }
    running_[id] = r;
    json s = state_locked();
    s[id]["last_used"] = now();
    save_state_locked(s);
    log::info(kLog, "started " + id);
    lock.unlock();
    notify();
    lock.lock();
    return {};
}

void AppManager::stop_locked(const std::string& id, const std::string& why) {
    const auto it = running_.find(id);
    if (it == running_.end()) return;
    std::shared_ptr<Running> r = std::move(it->second);
    running_.erase(it);
    json s = state_locked();
    const long long before = jobj(s, id.c_str()) ? jint(s[id], "last_used") : 0;
    s[id]["last_used"] = std::max(before, r->last_request);
    save_state_locked(s);
    append_log(id, "[client] stopping: " + why + "\n");
    r.reset();  // terminates the job and joins the reader
    const std::lock_guard lock(log_mx_);
    if (const auto l = logs_.find(id); l != logs_.end()) (void)write_file(env_.state_dir / "logs" / widen(id + ".log"), l->second);
    log::info(kLog, "stopped " + id + ": " + why);
}

Status AppManager::stop(const std::string& id) {
    {
        const std::lock_guard lock(mx_);
        reap_locked();
        if (!running_.count(id)) return {};
        stop_locked(id, "stopped by the user");
    }
    notify();
    return {};
}

Status AppManager::remove(const std::string& id) {
    {
        const std::lock_guard lock(mx_);
        auto a = info_locked(id);
        if (!a) return a.error();
        stop_locked(id, "deleted");
        // A terminated process releases its working folder and open files a moment after it ends.
        std::error_code ec;
        for (int attempt = 0; attempt < 40; ++attempt) {
            ec.clear();
            fs::remove_all(folder_of(id), ec);
            if (!ec) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if (ec) return make_error("io", "cannot delete the app folder: " + ec.message());
        json s = state_locked();
        s.erase(id);
        save_state_locked(s);
        for (auto& [_, ids] : grants_) ids.erase(id);
        fs::remove(env_.state_dir / "logs" / widen(id + ".log"), ec);
        const std::lock_guard l(log_mx_);
        logs_.erase(id);
    }
    log::info(kLog, "deleted " + id);
    notify();
    return {};
}

Result<std::shared_ptr<AppStream>> AppManager::connect(const std::string& id) {
    std::unique_lock lock(mx_);
    if (Status s = start_locked(lock, id); !s) return s.error();
    const auto it = running_.find(id);
    if (it == running_.end()) return make_error("stopped", "the app is not running");
    std::shared_ptr<Running> r = it->second;
    r->last_request = now();
    auto stream = std::make_shared<AppStream>();
    const long long sid = next_stream_++;
    std::weak_ptr<Running> weak = r;
    stream->write_ = [weak, sid](std::string_view type, std::string_view body) -> Status {
        auto run = weak.lock();
        if (!run) return make_error("stopped", "the app stopped");
        return run->write(type, sid, body);
    };
    {
        const std::lock_guard l(r->mx);
        if (r->ended) return make_error("stopped", "the app stopped");
        r->streams[sid] = stream;
    }
    if (Status s = r->write("open", sid, {}); !s) return s.error();
    return stream;
}

Result<json> AppManager::call_mcp(const std::string& id, const std::string& server, const std::string& tool, const json& args) {
    const std::string tool_id = "mcp:" + server + "/" + tool;
    if (mcp_mode() != "read") return make_error("policy_denied", "MCP data for Local apps is switched off by policy.apps.mcp.");
    {
        const std::lock_guard lock(mx_);
        auto a = info_locked(id);
        if (!a) return a.error();
        if (std::find(a->manifest.mcp_tools.begin(), a->manifest.mcp_tools.end(), tool_id) == a->manifest.mcp_tools.end())
            return make_error("not_listed", tool_id + " is not in the app's manifest.");
        if (a->needs_confirmation) return make_error("not_confirmed", "The user has not confirmed the MCP tools this app uses.");
        if (const auto r = running_.find(id); r != running_.end()) r->second->last_request = now();
    }
    const auto risk = env_.tool_risk ? env_.tool_risk(tool_id) : std::nullopt;
    if (!risk) return make_error("unavailable", tool_id + " is not available now.");
    if (*risk != runtime::Risk::read) return make_error("not_read", tool_id + " is not a read-risk tool; apps may only read.");
    if (!env_.call_tool) return make_error("unavailable", "MCP is not running.");
    return env_.call_tool(id, tool_id, args);
}

void AppManager::tick() {
    bool changed = false;
    {
        const std::lock_guard lock(mx_);
        reap_locked();
        const long long t = now();
        std::vector<std::string> idle;
        for (const auto& [id, r] : running_)
            if (t - r->last_request >= env_.idle.count()) idle.push_back(id);
        for (const auto& id : idle) stop_locked(id, "no request for " + std::to_string(env_.idle.count() / 60000) + " minutes");
        changed = !idle.empty();
    }
    if (changed) notify();
}

size_t AppManager::running_count() {
    const std::lock_guard lock(mx_);
    reap_locked();
    return running_.size();
}

void AppManager::shutdown() {
    const std::lock_guard lock(mx_);
    std::vector<std::string> ids;
    for (const auto& [id, _] : running_) ids.push_back(id);
    for (const auto& id : ids) stop_locked(id, "the client is shutting down");
}

}  // namespace wb::apps
