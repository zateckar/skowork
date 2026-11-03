#include "browser/service.h"

#include "base/encoding.h"
#include "base/strings.h"
#include "base/time.h"
#include "base/url.h"
#include "platform/dpapi.h"
#include "platform/log.h"
#include "platform/win.h"
#include "tools/quarantine.h"

#include <fstream>
#include <thread>

namespace wb::browser {

using runtime::Risk;
using runtime::ToolContext;
using runtime::ToolOutput;
using namespace std::chrono_literals;

namespace {

constexpr char kLog[] = "browser";
constexpr char kProxyUser[] = "workbench";

std::string stale_message(const std::string& ref) {
    return "stale reference " + ref + ": the page changed since the snapshot. Take a new snapshot with browser.snapshot "
           "and use its references.";
}

bool is_stale_error(const Error& e) {
    const std::string m = to_lower(e.message);
    return m.find("no node") != std::string::npos || m.find("could not find node") != std::string::npos ||
           m.find("node is detached") != std::string::npos || m.find("cannot find context") != std::string::npos;
}

// Reads an element's facts inside the client's isolated world, where page scripts cannot change the DOM methods.
constexpr char kFactsFn[] = R"(function(){const e=this;const g=(n)=>(e.getAttribute&&e.getAttribute(n))||'';
const f=e.form||(e.closest?e.closest('form'):null);
const t=(g('aria-label')||((e.tagName==='INPUT')?g('value'):'')||e.innerText||e.textContent||'').trim().slice(0,200);
return {tag:(e.tagName||'').toLowerCase(),type:g('type').toLowerCase(),role:g('role'),autocomplete:g('autocomplete').toLowerCase(),
name:t,in_form:!!f,form_has_action:!!(f&&f.getAttribute('action'))};})";

constexpr char kFocusFn[] = R"(function(clear){this.focus();if(clear&&('value' in this)){this.value='';
this.dispatchEvent(new Event('input',{bubbles:true}));}return true;})";

constexpr char kSelectFn[] = R"(function(want){if(this.tagName!=='SELECT')return {ok:false,error:'the element is not a select'};
const o=Array.from(this.options).find(x=>x.value===want||x.label===want||x.text.trim()===want);
if(!o)return {ok:false,error:'no option '+JSON.stringify(want)+'; options: '+Array.from(this.options).map(x=>x.label||x.text).slice(0,30).join(', ')};
this.value=o.value;this.dispatchEvent(new Event('input',{bubbles:true}));this.dispatchEvent(new Event('change',{bubbles:true}));
return {ok:true,selected:o.label||o.text,value:o.value};})";

}  // namespace

// ---- state ----------------------------------------------------------------------------------------------------------

struct BrowserService::Tab {
    std::string target_id, session, run_id;
    std::mutex mx;
    std::string url;
    RefTable refs;
    std::string blocked;  // the last top-level request refused by policy
    long long world = 0;  // isolated world of the current document
    int world_nav = -1;
    std::atomic<int> navigations{0};
    std::atomic<bool> closed{false};
};

struct BrowserService::RunTabs {
    std::vector<std::shared_ptr<Tab>> tabs;
    std::size_t active = 0;
    std::string start;  // registrable domain of the Run's starting site
};

struct BrowserService::Process {
    std::unique_ptr<ChromeProcess> chrome;
    std::unique_ptr<sandbox::EgressProxy::Session> proxy;
    bool headless = false;
    std::filesystem::path staging;
    std::mutex mx;
    std::condition_variable attached_cv;
    std::map<std::string, std::shared_ptr<Tab>> by_session;  // page sessions of Run tabs
    std::map<std::string, std::string> session_of_target;    // attached page targets
    std::map<std::string, std::string> run_of_session;       // every attached session of a Run's pages
    std::map<std::string, std::string> run_of_target;        // Run tab targets (popups inherit the opener's Run)
};

BrowserService::BrowserService(BrowserEnv env) : env_(std::move(env)) {}

BrowserService::~BrowserService() { shutdown(); }

void BrowserService::shutdown() { close_browser("shutdown"); }

bool BrowserService::policy_bool(const char* key, bool fallback) {
    const auto p = env_.policy ? env_.policy() : nullptr;
    const json* v = p ? p->find(key) : nullptr;
    return v && v->is_boolean() ? v->get<bool>() : fallback;
}

long long BrowserService::policy_int(const char* key, long long fallback) {
    const auto p = env_.policy ? env_.policy() : nullptr;
    return p ? p->integer(key, fallback) : fallback;
}

BrowserService::State BrowserService::state() {
    const auto now = std::chrono::steady_clock::now();
    std::shared_ptr<Process> p;
    {
        const std::lock_guard lock(mx_);
        p = proc_;
        if (!checked_ || now - checked_at_ > 60s) {
            State s;
            auto info = find_chrome(env_.chrome_exe);
            if (!info) {
                s.state = "unavailable";
                s.reason = info.error().message;
            } else {
                s.version = info->version;
                const auto pol = read_chrome_policy(env_.chrome_policy_key);
                if (!pol.allowed) {
                    s.state = "blocked";
                    s.policy = pol.policy;
                    s.reason = "Browser use blocked by Chrome policy " + pol.policy;
                } else {
                    s.state = "available";
                }
            }
            checked_ = s;
            checked_at_ = now;
        }
    }
    State s = *checked_;
    s.running = p && p->chrome && p->chrome->running();
    s.headless = s.running && p->headless;
    return s;
}

bool BrowserService::offered() { return state().state == "available"; }

bool BrowserService::eval_offered() { return offered() && policy_bool("tools.browser.allow_eval", false); }

void BrowserService::touch() {
    const std::lock_guard lock(mx_);
    last_call_ = std::chrono::steady_clock::now();
}

unsigned long BrowserService::chrome_pid() {
    const std::lock_guard lock(mx_);
    return proc_ && proc_->chrome ? proc_->chrome->pid() : 0;
}

std::string BrowserService::active_target(const std::string& run_id) {
    const std::lock_guard lock(mx_);
    auto it = runs_.find(run_id);
    if (it == runs_.end() || it->second->tabs.empty()) return {};
    return it->second->tabs[it->second->active]->target_id;
}

std::string BrowserService::start_site(const std::string& run_id) {
    const std::lock_guard lock(mx_);
    auto it = runs_.find(run_id);
    return it == runs_.end() ? std::string{} : it->second->start;
}

long long BrowserService::download_mark() {
    const std::lock_guard lock(dl_mx_);
    return dl_seq_;
}

// ---- process ------------------------------------------------------------------------------------------------------

void BrowserService::close_browser(const std::string& why) {
    std::shared_ptr<Process> p;
    {
        const std::lock_guard lock(mx_);
        p.swap(proc_);
        runs_.clear();
    }
    if (!p) return;
    if (p->chrome) {
        log::info(kLog, "closing the agent browser (" + why + ")");
        p->chrome->terminate();
    }
    p->proxy.reset();
}

bool BrowserService::user_interacting(unsigned long pid) {
    HWND fg = ::GetForegroundWindow();
    DWORD fpid = 0;
    if (fg) ::GetWindowThreadProcessId(fg, &fpid);
    if (fpid != pid) return false;
    LASTINPUTINFO li{sizeof li, 0};
    if (!::GetLastInputInfo(&li)) return true;
    return std::chrono::milliseconds(::GetTickCount() - li.dwTime) < env_.user_input_window;
}

void BrowserService::tick() {
    std::shared_ptr<Process> p;
    std::chrono::steady_clock::time_point last;
    {
        const std::lock_guard lock(mx_);
        p = proc_;
        last = last_call_;
    }
    if (!p || !p->chrome) return;
    if (!p->chrome->running()) {
        close_browser("Chrome exited");
        return;
    }
    const auto idle = env_.idle_timeout.count() > 0
                          ? env_.idle_timeout
                          : std::chrono::milliseconds(std::chrono::minutes(std::max<long long>(1, policy_int("tools.browser.idle_timeout_minutes", 15))));
    if (std::chrono::steady_clock::now() - last < idle) return;
    if (user_interacting(p->chrome->pid())) return;  // never close it under the user's hands
    close_browser("idle");
}

void BrowserService::run_ended(const std::string& run_id) {
    std::shared_ptr<RunTabs> rt;
    std::shared_ptr<Process> p;
    {
        const std::lock_guard lock(mx_);
        auto it = runs_.find(run_id);
        if (it == runs_.end()) return;
        rt = it->second;
        runs_.erase(it);
        p = proc_;
    }
    if (!p || !p->chrome) return;
    for (const auto& t : rt->tabs) {
        if (t->closed) continue;
        p->chrome->cdp().send("Target.closeTarget", {{"targetId", t->target_id}});
    }
}

Result<std::shared_ptr<BrowserService::Process>> BrowserService::ensure_browser(bool unattended) {
    const std::lock_guard start_lock(start_mx_);
    const bool want_headless = env_.force_headless || (unattended && policy_bool("tools.browser.headless_unattended", false));
    {
        std::unique_lock lock(mx_);
        if (proc_ && proc_->chrome && proc_->chrome->running() && proc_->chrome->cdp().alive()) {
            bool tabs_in_use = false;
            for (const auto& [id, rt] : runs_) tabs_in_use |= !rt->tabs.empty();
            // An attended Run needs a visible window: restart a headless browser nobody else is using.
            if (!(proc_->headless && !want_headless && !tabs_in_use)) return proc_;
        }
    }
    close_browser("restart");
    const State st = state();
    if (st.state != "available") return make_error("unavailable", st.reason);
    auto info = find_chrome(env_.chrome_exe);
    if (!info) return info.error();

    auto p = std::make_shared<Process>();
    p->headless = want_headless;
    LaunchOptions o;
    o.exe = info->exe;
    o.profile_dir = env_.data_root / L"browser-profile";
    o.headless = want_headless;
    o.extra_args = env_.extra_chrome_args;
    if (env_.proxy) {
        auto s = env_.proxy->open(sandbox::EgressProxy::Owner{"", "", "browser"});
        if (!s) return make_error("unavailable", "the egress proxy could not be started: " + s.error().message);
        p->proxy = std::move(*s);
        o.proxy_server = "http://127.0.0.1:" + std::to_string(p->proxy->port());
    }
    p->staging = env_.data_root / L"browser-downloads";
    std::error_code ec;
    std::filesystem::create_directories(p->staging, ec);

    std::weak_ptr<Process> wp = p;
    auto chrome = launch_chrome(o, [this, wp](const CdpMessage& m) { on_event(wp, m); });
    if (!chrome) {
        const std::lock_guard lock(mx_);
        // The handshake is the second half of the policy check (design D6): withdraw the tools until the next check.
        checked_ = State{"unavailable", chrome.error().message, "", info->version, false, false};
        checked_at_ = std::chrono::steady_clock::now();
        return chrome.error();
    }
    p->chrome = std::move(*chrome);
    auto& cdp = p->chrome->cdp();
    if (auto r = cdp.call("Target.setAutoAttach", {{"autoAttach", true}, {"waitForDebuggerOnStart", true}, {"flatten", true}}); !r)
        log::warn(kLog, "auto-attach: " + r.error().message);
    if (auto r = cdp.call("Browser.setDownloadBehavior", {{"behavior", "allowAndName"},
                                                          {"downloadPath", narrow(p->staging.wstring())},
                                                          {"eventsEnabled", true}});
        !r)
        log::warn(kLog, "download behaviour: " + r.error().message);
    {
        const std::lock_guard lock(mx_);
        proc_ = p;
        runs_.clear();
        last_call_ = std::chrono::steady_clock::now();
    }
    return p;
}

// ---- events (the connection's event thread) -------------------------------------------------------------------------

void BrowserService::on_event(const std::weak_ptr<Process>& wp, const CdpMessage& m) {
    auto p = wp.lock();
    // Auto-attach starts after launch_chrome returned, so every attached target finds the process.
    if (!p || !p->chrome) return;
    if (m.method == "Target.attachedToTarget") {
        on_attached(p, m);
    } else if (m.method == "Fetch.requestPaused") {
        on_request(p, m);
    } else if (m.method == "Fetch.authRequired") {
        const std::string id = jstr(m.params, "requestId");
        const json& ch = m.params.contains("authChallenge") ? m.params["authChallenge"] : json::object();
        json resp = {{"response", "Default"}};  // a site's own sign-in stays with the user
        if (jstr(ch, "source") == "Proxy" && p->proxy)
            resp = {{"response", "ProvideCredentials"}, {"username", kProxyUser}, {"password", p->proxy->credential()}};
        p->chrome->cdp().send("Fetch.continueWithAuth", {{"requestId", id}, {"authChallengeResponse", resp}}, m.session_id);
    } else if (m.method == "Page.frameNavigated") {
        const json& f = m.params.contains("frame") ? m.params["frame"] : json::object();
        if (!jstr(f, "parentId").empty()) return;
        std::shared_ptr<Tab> tab;
        {
            const std::lock_guard lock(p->mx);
            auto it = p->by_session.find(m.session_id);
            if (it != p->by_session.end()) tab = it->second;
        }
        if (!tab) return;
        const std::lock_guard lock(tab->mx);
        tab->url = jstr(f, "url");
        tab->refs.reset();
        ++tab->navigations;
    } else if (m.method == "Target.detachedFromTarget") {
        const std::string s = jstr(m.params, "sessionId");
        std::shared_ptr<Tab> tab;
        {
            const std::lock_guard lock(p->mx);
            auto it = p->by_session.find(s);
            if (it != p->by_session.end()) {
                tab = it->second;
                p->by_session.erase(it);
            }
            p->run_of_session.erase(s);
        }
        if (tab) {
            tab->closed = true;
            const std::lock_guard lock(mx_);
            if (auto it = runs_.find(tab->run_id); it != runs_.end()) {
                auto& tabs = it->second->tabs;
                for (std::size_t i = 0; i < tabs.size(); ++i)
                    if (tabs[i] == tab) {
                        tabs.erase(tabs.begin() + static_cast<std::ptrdiff_t>(i));
                        if (it->second->active >= tabs.size()) it->second->active = tabs.empty() ? 0 : tabs.size() - 1;
                        break;
                    }
            }
        }
    } else if (m.method == "Browser.downloadWillBegin" || m.method == "Browser.downloadProgress") {
        on_download(m);
    }
}

void BrowserService::on_attached(const std::shared_ptr<Process>& p, const CdpMessage& m) {
    const std::string s = jstr(m.params, "sessionId");
    const json& ti = m.params.contains("targetInfo") ? m.params["targetInfo"] : json::object();
    const std::string type = jstr(ti, "type"), target = jstr(ti, "targetId"), opener = jstr(ti, "openerId");
    auto& cdp = p->chrome->cdp();
    std::string run;
    bool popup = false;
    {
        const std::lock_guard lock(p->mx);
        if (!m.session_id.empty()) run = p->run_of_session[m.session_id];  // a frame or worker of a Run's page
        if (run.empty() && !opener.empty())
            if (auto it = p->run_of_target.find(opener); it != p->run_of_target.end()) {
                run = it->second;
                popup = type == "page";
            }
        p->run_of_session[s] = run;
    }
    // Every request of the target goes through the egress check (design D5) before the target runs.
    if (auto r = cdp.call("Fetch.enable", {{"patterns", json::array({{{"urlPattern", "*"}, {"requestStage", "Request"}}})},
                                           {"handleAuthRequests", true}},
                          s, 10s);
        !r && type != "service_worker" && type != "shared_worker")
        log::warn(kLog, "request interception on a " + type + ": " + r.error().message);
    if (type == "page" || type == "iframe") {
        (void)cdp.call("Target.setAutoAttach", {{"autoAttach", true}, {"waitForDebuggerOnStart", true}, {"flatten", true}}, s, 10s);
        (void)cdp.call("Page.enable", json::object(), s, 10s);
    }
    if (jbool(m.params, "waitingForDebugger")) cdp.send("Runtime.runIfWaitingForDebugger", json::object(), s);
    if (popup) {
        auto tab = std::make_shared<Tab>();
        tab->target_id = target;
        tab->session = s;
        tab->run_id = run;
        tab->url = jstr(ti, "url");
        {
            const std::lock_guard lock(p->mx);
            p->by_session[s] = tab;
            p->run_of_target[target] = run;
        }
        const std::lock_guard lock(mx_);
        if (auto it = runs_.find(run); it != runs_.end()) {
            it->second->tabs.push_back(tab);
            it->second->active = it->second->tabs.size() - 1;  // the page opened it for the agent: work there
        }
    }
    {
        const std::lock_guard lock(p->mx);
        if (type == "page") p->session_of_target[target] = s;
    }
    p->attached_cv.notify_all();
}

void BrowserService::on_request(const std::shared_ptr<Process>& p, const CdpMessage& m) {
    const std::string id = jstr(m.params, "requestId");
    const json& req = m.params.contains("request") ? m.params["request"] : json::object();
    const std::string url = jstr(req, "url");
    const auto pol = env_.policy ? env_.policy() : nullptr;
    const policy::RuleVerdict v = browser_request_verdict(pol.get(), url);
    auto& cdp = p->chrome->cdp();
    if (v.allowed) {
        cdp.send("Fetch.continueRequest", {{"requestId", id}}, m.session_id);
        return;
    }
    cdp.send("Fetch.failRequest", {{"requestId", id}, {"errorReason", "BlockedByClient"}}, m.session_id);
    std::string run;
    std::shared_ptr<Tab> tab;
    {
        const std::lock_guard lock(p->mx);
        run = p->run_of_session[m.session_id];
        if (auto it = p->by_session.find(m.session_id); it != p->by_session.end()) tab = it->second;
    }
    if (tab && jstr(m.params, "resourceType") == "Document") {
        const std::lock_guard lock(tab->mx);
        tab->blocked = url + " was blocked by policy: " + v.reason + " (" + v.rule + ")";
    }
    log::info(kLog, "browser request refused: " + url_host(url) + " (" + v.rule + ")");
    if (env_.on_denied) env_.on_denied(run, url, v);
}

void BrowserService::on_download(const CdpMessage& m) {
    const std::string guid = jstr(m.params, "guid");
    if (guid.empty()) return;
    std::unique_lock lock(dl_mx_);
    Download& d = downloads_[guid];
    d.guid = guid;
    if (m.method == "Browser.downloadWillBegin") {
        d.url = jstr(m.params, "url");
        d.name = jstr(m.params, "suggestedFilename");
        d.seq = ++dl_seq_;
        return;
    }
    const std::string state = jstr(m.params, "state");
    if (state == "canceled") {
        d.done = true;
        d.error = "the download was cancelled";
    } else if (state == "completed") {
        std::filesystem::path staging;
        {
            const std::lock_guard plock(mx_);
            if (proc_) staging = proc_->staging;
        }
        const auto file = staging / widen(guid);
        std::ifstream in(file, std::ios::binary);
        std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        in.close();
        std::error_code ec;
        std::filesystem::remove(file, ec);
        auto saved = tools::quarantine_save(env_.quarantine_dir, d.name.empty() ? tools::download_name("", d.url) : d.name, bytes, d.url);
        if (saved) d.path = saved->path;
        else d.error = saved.error().message;
        d.done = true;
    } else {
        return;
    }
    lock.unlock();
    dl_cv_.notify_all();
}

// ---- tabs ---------------------------------------------------------------------------------------------------------

Result<std::shared_ptr<BrowserService::Tab>> BrowserService::open_tab(const std::shared_ptr<Process>& p, const std::string& run_id,
                                                                       const std::string& url) {
    auto& cdp = p->chrome->cdp();
    auto created = cdp.call("Target.createTarget", {{"url", url.empty() ? std::string("about:blank") : url}});
    if (!created) return created.error();
    const std::string target = jstr(*created, "targetId");
    std::string session;
    {
        std::unique_lock lock(p->mx);
        p->run_of_target[target] = run_id;
        p->attached_cv.wait_for(lock, 10s, [&] { return p->session_of_target.count(target) > 0; });
        if (auto it = p->session_of_target.find(target); it != p->session_of_target.end()) session = it->second;
    }
    if (session.empty()) {
        auto a = cdp.call("Target.attachToTarget", {{"targetId", target}, {"flatten", true}});
        if (!a) return a.error();
        std::unique_lock lock(p->mx);
        p->attached_cv.wait_for(lock, 10s, [&] { return p->session_of_target.count(target) > 0; });
        session = jstr(*a, "sessionId");
    }
    auto tab = std::make_shared<Tab>();
    tab->target_id = target;
    tab->session = session;
    tab->run_id = run_id;
    tab->url = url.empty() ? "about:blank" : url;
    {
        const std::lock_guard lock(p->mx);
        p->by_session[session] = tab;
        p->run_of_session[session] = run_id;
    }
    (void)cdp.call("Accessibility.enable", json::object(), session, 10s);
    if (!p->headless) (void)cdp.call("Page.bringToFront", json::object(), session, 5s);
    return tab;
}

Result<std::shared_ptr<BrowserService::Tab>> BrowserService::run_tab(ToolContext& ctx, bool create) {
    touch();
    std::shared_ptr<Process> p;
    if (create) {
        auto e = ensure_browser(ctx.unattended);
        if (!e) return e.error();
        p = *e;
    }
    {
        const std::lock_guard lock(mx_);
        if (!p) p = proc_;
        if (p && p->chrome && p->chrome->running()) {
            auto it = runs_.find(ctx.run_id);
            if (it != runs_.end() && !it->second->tabs.empty()) {
                auto& rt = *it->second;
                if (rt.active >= rt.tabs.size()) rt.active = rt.tabs.size() - 1;
                if (!rt.tabs[rt.active]->closed) return rt.tabs[rt.active];
            }
        }
    }
    if (!create) return make_error("no_page", "No page is open in this Run. Use browser.navigate first.");
    auto tab = open_tab(p, ctx.run_id, "about:blank");
    if (!tab) return tab.error();
    const std::lock_guard lock(mx_);
    auto& rt = runs_[ctx.run_id];
    if (!rt) rt = std::make_shared<RunTabs>();
    rt->tabs.push_back(*tab);
    rt->active = rt->tabs.size() - 1;
    return *tab;
}

std::string BrowserService::page_url(const std::shared_ptr<Tab>& tab) {
    const std::lock_guard lock(tab->mx);
    return tab->url;
}

// ---- element access -------------------------------------------------------------------------------------------------

Result<std::string> BrowserService::object_for(const std::shared_ptr<Process>& p, const std::shared_ptr<Tab>& tab, const std::string& ref) {
    std::optional<long long> backend;
    long long world = 0;
    int world_nav = -1;
    {
        const std::lock_guard lock(tab->mx);
        backend = tab->refs.resolve(ref);
        world = tab->world;
        world_nav = tab->world_nav;
    }
    if (!backend) return make_error("stale_reference", stale_message(ref));
    auto& cdp = p->chrome->cdp();
    const int nav = tab->navigations.load();
    if (world == 0 || world_nav != nav) {
        // The client's isolated world in the top frame of the current document.
        world = 0;
        if (auto tree = cdp.call("Page.getFrameTree", json::object(), tab->session, 10s)) {
            const std::string frame = jstr((*tree)["frameTree"]["frame"], "id");
            if (auto w = cdp.call("Page.createIsolatedWorld", {{"frameId", frame}, {"worldName", "workbench"}}, tab->session, 10s))
                world = jint(*w, "executionContextId");
        }
        const std::lock_guard lock(tab->mx);
        tab->world = world;
        tab->world_nav = nav;
    }
    json params = {{"backendNodeId", *backend}};
    if (world) params["executionContextId"] = world;
    auto r = cdp.call("DOM.resolveNode", params, tab->session, 10s);
    if (!r && world) r = cdp.call("DOM.resolveNode", {{"backendNodeId", *backend}}, tab->session, 10s);  // inside a frame
    if (!r) return is_stale_error(r.error()) ? make_error("stale_reference", stale_message(ref)) : r.error();
    const std::string object = jstr((*r)["object"], "objectId");
    if (object.empty()) return make_error("stale_reference", stale_message(ref));
    return object;
}

Result<json> BrowserService::call_on(const std::shared_ptr<Process>& p, const std::shared_ptr<Tab>& tab, const std::string& ref,
                                     const std::string& function, const json& arguments) {
    auto object = object_for(p, tab, ref);
    if (!object) return object.error();
    json args = json::array();
    for (const auto& a : arguments) args.push_back({{"value", a}});
    auto r = p->chrome->cdp().call("Runtime.callFunctionOn",
                                   {{"objectId", *object}, {"functionDeclaration", function}, {"arguments", args}, {"returnByValue", true}},
                                   tab->session, 15s);
    if (!r) return is_stale_error(r.error()) ? make_error("stale_reference", stale_message(ref)) : r.error();
    if (r->contains("exceptionDetails")) return make_error("page", "the page raised an error");
    return (*r)["result"].contains("value") ? (*r)["result"]["value"] : json(nullptr);
}

Result<ElementFacts> BrowserService::facts(const std::shared_ptr<Process>& p, const std::shared_ptr<Tab>& tab, const std::string& ref) {
    auto v = call_on(p, tab, ref, kFactsFn);
    if (!v) return v.error();
    ElementFacts f;
    f.tag = jstr(*v, "tag");
    f.type = jstr(*v, "type");
    f.role = jstr(*v, "role");
    f.name = jstr(*v, "name");
    f.autocomplete = jstr(*v, "autocomplete");
    f.in_form = jbool(*v, "in_form");
    f.form_has_action = jbool(*v, "form_has_action");
    return f;
}

json BrowserService::settle(const std::shared_ptr<Process>& p, const std::shared_ptr<Tab>& tab, int navigations_before, long long download_seq,
                            std::chrono::milliseconds max_wait) {
    auto& cdp = p->chrome->cdp();
    const auto start = std::chrono::steady_clock::now();
    // Give a click time to start a navigation, then wait for the document to load.
    while (tab->navigations.load() == navigations_before && std::chrono::steady_clock::now() - start < 400ms)
        std::this_thread::sleep_for(25ms);
    while (std::chrono::steady_clock::now() - start < max_wait) {
        auto r = cdp.call("Runtime.evaluate", {{"expression", "document.readyState"}, {"returnByValue", true}}, tab->session, 5s);
        if (r && jstr((*r)["result"], "value") == "complete") break;
        std::this_thread::sleep_for(100ms);
    }
    json out = {{"url", page_url(tab)}, {"navigated", tab->navigations.load() != navigations_before}};
    if (auto t = cdp.call("Runtime.evaluate", {{"expression", "document.title"}, {"returnByValue", true}}, tab->session, 5s))
        out["title"] = jstr((*t)["result"], "value");
    // Downloads this action started: wait for them to land in the quarantine.
    std::unique_lock lock(dl_mx_);
    const auto began = [&] {
        std::vector<std::string> g;
        for (const auto& [id, d] : downloads_)
            if (d.seq > download_seq) g.push_back(id);
        return g;
    };
    auto ids = began();
    if (ids.empty()) {
        lock.unlock();
        std::this_thread::sleep_for(150ms);  // downloadWillBegin can trail the click
        lock.lock();
        ids = began();
    }
    if (!ids.empty()) {
        dl_cv_.wait_for(lock, 10s, [&] {
            for (const auto& id : ids)
                if (!downloads_[id].done) return false;
            return true;
        });
        json list = json::array();
        for (const auto& id : ids) {
            const Download& d = downloads_[id];
            if (!d.path.empty()) list.push_back({{"file", d.path}, {"from", d.url}});
            else list.push_back({{"from", d.url}, {"error", d.done ? d.error : std::string("still downloading")}});
        }
        out["downloads"] = list;
    }
    return out;
}

ToolOutput BrowserService::result(const std::shared_ptr<Tab>& tab, json body, bool ok) {
    std::string text = body.dump(2, ' ', false, json::error_handler_t::replace);
    ToolOutput out = ok ? ToolOutput::success(std::move(text)) : ToolOutput::failure(std::move(text));
    out.provenance = runtime::Provenance{"browser", page_url(tab), iso8601_utc_now(), false};
    return out;
}

ToolOutput BrowserService::handover(const std::shared_ptr<Process>& p, const std::shared_ptr<Tab>& tab, const std::string& why) {
    if (!p->headless) {
        (void)p->chrome->cdp().call("Page.bringToFront", json::object(), tab->session, 5s);
        // Bring the agent browser's window to the front for the user.
        struct Find {
            DWORD pid;
            HWND hwnd = nullptr;
        } f{static_cast<DWORD>(p->chrome->pid())};
        ::EnumWindows(
            [](HWND h, LPARAM lp) -> BOOL {
                auto* x = reinterpret_cast<Find*>(lp);
                DWORD pid = 0;
                ::GetWindowThreadProcessId(h, &pid);
                if (pid == x->pid && ::IsWindowVisible(h) && !::GetWindow(h, GW_OWNER)) {
                    x->hwnd = h;
                    return FALSE;
                }
                return TRUE;
            },
            reinterpret_cast<LPARAM>(&f));
        if (f.hwnd) {
            if (::IsIconic(f.hwnd)) ::ShowWindow(f.hwnd, SW_RESTORE);
            ::SetForegroundWindow(f.hwnd);
        }
    }
    runtime::QuestionRequest q;
    q.prompt = p->headless ? "Please sign in in the agent browser, then continue (it runs without a window for this automation; "
                             "choose Stop and run the task while you are present)"
                           : "Please sign in in the agent browser, then continue";
    q.choices = {"Continue", "Stop"};
    q.free_text = false;
    q.state = {{"reason", why}, {"url", page_url(tab)}};
    return ToolOutput::ask(std::move(q));
}

ToolOutput BrowserService::resume_handover(ToolContext& ctx, const json&, const json& state, const std::string& answer) {
    touch();
    if (answer == "Continue")
        return ToolOutput::success("The user finished in the agent browser (" + jstr(state, "reason") +
                                   "). Take a new snapshot before you continue; do not try to type credentials.");
    if (ctx.control) ctx.control->cancel("Stopped by the user at the sign-in in the agent browser");
    return ToolOutput::failure("The user chose Stop.");
}

// ---- risk ---------------------------------------------------------------------------------------------------------

Risk BrowserService::risk(const std::string& tool, const std::string& run_id, const json& args) {
    if (tool == "browser.eval") return Risk::destructive;
    if (tool == "browser.snapshot" || tool == "browser.screenshot" || tool == "browser.wait" || tool == "browser.scroll") return Risk::read;
    const std::string start = start_site(run_id);
    const auto off_site = [&](const std::string& url) {
        const std::string host = url_host(url);
        return !start.empty() && !host.empty() && registrable_domain(host) != start;
    };
    if (tool == "browser.navigate") return off_site(jstr(args, "url")) ? Risk::external : Risk::write_local;
    if (tool == "browser.tabs") {
        const std::string action = jstr(args, "action", "list");
        if (action == "list" || action == "switch") return Risk::read;
        if (action == "open") return off_site(jstr(args, "url")) ? Risk::external : Risk::write_local;
        return Risk::write_local;
    }
    // Actions on the current page.
    std::shared_ptr<Tab> tab;
    std::shared_ptr<Process> p;
    {
        const std::lock_guard lock(mx_);
        p = proc_;
        auto it = runs_.find(run_id);
        if (it != runs_.end() && !it->second->tabs.empty()) tab = it->second->tabs[std::min(it->second->active, it->second->tabs.size() - 1)];
    }
    if (!tab || !p || !p->chrome) return Risk::write_local;
    if (off_site(page_url(tab))) return Risk::external;
    if (tool == "browser.type" && jbool(args, "submit")) return Risk::external;
    if (tool == "browser.click") {
        auto f = facts(p, tab, jstr(args, "ref"));
        if (f && is_submitting(*f)) return Risk::external;
    }
    return Risk::write_local;
}

// ---- tools --------------------------------------------------------------------------------------------------------

ToolOutput BrowserService::snapshot(ToolContext& ctx, const json&) {
    auto tab = run_tab(ctx, false);
    if (!tab) return ToolOutput::failure(tab.error().message);
    std::shared_ptr<Process> p;
    {
        const std::lock_guard lock(mx_);
        p = proc_;
    }
    if (!p) return ToolOutput::failure("The agent browser is not running.");
    auto& cdp = p->chrome->cdp();
    auto tree = cdp.call("Accessibility.getFullAXTree", json::object(), (*tab)->session, 30s);
    if (!tree) return ToolOutput::failure("The page could not be read: " + tree.error().message);
    std::string title;
    if (auto t = cdp.call("Runtime.evaluate", {{"expression", "document.title"}, {"returnByValue", true}}, (*tab)->session, 5s))
        title = jstr((*t)["result"], "value");
    Snapshot snap;
    std::string url;
    {
        const std::lock_guard lock((*tab)->mx);
        snap = build_snapshot((*tree)["nodes"], (*tab)->refs);
        url = (*tab)->url;
    }
    ToolOutput out = ToolOutput::success("Page: " + title + "\nURL: " + url + "\nElements with [ref=...] can be used with the "
                                         "browser action tools.\n\n" + snap.text);
    out.provenance = runtime::Provenance{"browser", url, iso8601_utc_now(), false};
    return out;
}

ToolOutput BrowserService::screenshot(ToolContext& ctx, const json& args) {
    auto tab = run_tab(ctx, false);
    if (!tab) return ToolOutput::failure(tab.error().message);
    std::shared_ptr<Process> p;
    {
        const std::lock_guard lock(mx_);
        p = proc_;
    }
    if (!p) return ToolOutput::failure("The agent browser is not running.");
    auto& cdp = p->chrome->cdp();
    const bool full = jbool(args, "full_page");
    json params = {{"format", "png"}};
    if (full) {
        if (auto m = cdp.call("Page.getLayoutMetrics", json::object(), (*tab)->session, 10s)) {
            const json& size = (*m).contains("cssContentSize") ? (*m)["cssContentSize"] : (*m)["contentSize"];
            const double w = size.value("width", 1280.0), h = std::min(size.value("height", 800.0), 16384.0);
            params["clip"] = {{"x", 0}, {"y", 0}, {"width", w}, {"height", h}, {"scale", 1}};
            params["captureBeyondViewport"] = true;
        }
    }
    auto shot = cdp.call("Page.captureScreenshot", params, (*tab)->session, 30s);
    if (!shot) return ToolOutput::failure("The screenshot failed: " + shot.error().message);
    auto png = base64_decode(jstr(*shot, "data"));
    if (!png || png->empty()) return ToolOutput::failure("The screenshot failed: no image data.");
    if (!env_.prepare_image) return ToolOutput::failure("Images cannot be sent to the model in this configuration.");
    auto img = env_.prepare_image(*png);
    if (!img) return ToolOutput::failure("The screenshot could not be prepared: " + img.error().message);
    if (policy_bool("tools.browser.retain_screenshots", false)) {
        // Kept only when policy allows it, encrypted for the current user (design D7).
        auto blob = dpapi_protect(*png, L"workbench-screenshot");
        if (blob) {
            const auto dir = env_.data_root / L"screenshots" / widen(ctx.run_id);
            std::error_code ec;
            std::filesystem::create_directories(dir, ec);
            std::ofstream(dir / widen(ctx.call_id + ".png.dpapi"), std::ios::binary)
                .write(reinterpret_cast<const char*>(blob->data()), static_cast<std::streamsize>(blob->size()));
        }
    }
    const std::string url = page_url(*tab);
    ToolOutput out = ToolOutput::success(std::string("Screenshot of ") + (full ? "the full page " : "the visible part of ") + url);
    out.images.push_back(std::move(*img));
    out.provenance = runtime::Provenance{"browser", url, iso8601_utc_now(), false};
    return out;
}

ToolOutput BrowserService::navigate(ToolContext& ctx, const json& args) {
    const std::string url = jstr(args, "url");
    const auto u = parse_url(url);
    if (!u || (u->scheme != "http" && u->scheme != "https")) return ToolOutput::failure("browser.navigate needs an http or https URL.");
    auto tab = run_tab(ctx, true);
    if (!tab) return ToolOutput::failure("The agent browser is unavailable: " + tab.error().message);
    std::shared_ptr<Process> p;
    {
        const std::lock_guard lock(mx_);
        p = proc_;
        auto& rt = runs_[ctx.run_id];
        if (!rt) rt = std::make_shared<RunTabs>();
        if (rt->start.empty()) rt->start = registrable_domain(u->host);  // the Run's starting site (design D8)
    }
    {
        const std::lock_guard lock((*tab)->mx);
        (*tab)->blocked.clear();
    }
    long long dl = 0;
    {
        const std::lock_guard lock(dl_mx_);
        dl = dl_seq_;
    }
    const int before = (*tab)->navigations.load();
    auto r = p->chrome->cdp().call("Page.navigate", {{"url", url}}, (*tab)->session, env_.navigation_timeout);
    if (!r && r.error().code == "timeout") {
        // Chrome holds a navigation while it waits for the user, for example in its client certificate dialog, which
        // CDP cannot see or answer (design D11).
        if (!p->headless) return handover(p, *tab, "the page is waiting for you, for example for a certificate choice");
        return ToolOutput::failure("The page did not load: it may be waiting for a client certificate choice, which needs the "
                                   "agent browser's window. Run the task while you are present.");
    }
    if (!r) return ToolOutput::failure("Navigation failed: " + r.error().message);
    const std::string error = jstr(*r, "errorText");
    if (!error.empty()) {
        std::string blocked;
        {
            const std::lock_guard lock((*tab)->mx);
            blocked = (*tab)->blocked;
        }
        if (!blocked.empty() || error.find("BLOCKED_BY_CLIENT") != std::string::npos)
            return result(*tab, {{"url", url}, {"navigated", false}, {"error", blocked.empty() ? url + " was blocked by policy" : blocked}}, false);
        if (error.find("ABORTED") == std::string::npos)  // ERR_ABORTED: a download or a replaced navigation
            return result(*tab, {{"url", url}, {"navigated", false}, {"error", error}}, false);
    }
    json body = settle(p, *tab, before, dl, 30s);
    if (is_sign_in_host(url_host(jstr(body, "url")), env_.idp_hosts)) return handover(p, *tab, "the page asks the user to sign in");
    return result(*tab, std::move(body));
}

ToolOutput BrowserService::click(ToolContext& ctx, const json& args) {
    auto tab = run_tab(ctx, false);
    if (!tab) return ToolOutput::failure(tab.error().message);
    std::shared_ptr<Process> p;
    {
        const std::lock_guard lock(mx_);
        p = proc_;
    }
    const std::string ref = jstr(args, "ref");
    auto f = facts(p, *tab, ref);
    if (!f) return ToolOutput::failure(f.error().message);
    std::optional<long long> backend;
    {
        const std::lock_guard lock((*tab)->mx);
        backend = (*tab)->refs.resolve(ref);
    }
    if (!backend) return ToolOutput::failure(stale_message(ref));
    auto& cdp = p->chrome->cdp();
    (void)cdp.call("DOM.scrollIntoViewIfNeeded", {{"backendNodeId", *backend}}, (*tab)->session, 10s);
    auto quads = cdp.call("DOM.getContentQuads", {{"backendNodeId", *backend}}, (*tab)->session, 10s);
    if (!quads) return ToolOutput::failure(is_stale_error(quads.error()) ? stale_message(ref) : "The element cannot be clicked: " + quads.error().message);
    const json& q = (*quads)["quads"];
    if (!q.is_array() || q.empty() || !q[0].is_array() || q[0].size() < 8)
        return ToolOutput::failure("The element " + ref + " is not visible, so it cannot be clicked.");
    double x = 0, y = 0;
    for (std::size_t i = 0; i < 8; i += 2) {
        x += q[0][i].get<double>() / 4;
        y += q[0][i + 1].get<double>() / 4;
    }
    long long dl = 0;
    {
        const std::lock_guard lock(dl_mx_);
        dl = dl_seq_;
    }
    const int before = (*tab)->navigations.load();
    (void)cdp.call("Input.dispatchMouseEvent", {{"type", "mouseMoved"}, {"x", x}, {"y", y}}, (*tab)->session, 10s);
    (void)cdp.call("Input.dispatchMouseEvent", {{"type", "mousePressed"}, {"x", x}, {"y", y}, {"button", "left"}, {"clickCount", 1}}, (*tab)->session, 10s);
    auto up = cdp.call("Input.dispatchMouseEvent", {{"type", "mouseReleased"}, {"x", x}, {"y", y}, {"button", "left"}, {"clickCount", 1}},
                       (*tab)->session, 10s);
    if (!up) return ToolOutput::failure("The click failed: " + up.error().message);
    json body = settle(p, *tab, before, dl, 15s);
    body["clicked"] = f->name.empty() ? ref : f->name;
    if (body["navigated"].get<bool>() && is_sign_in_host(url_host(jstr(body, "url")), env_.idp_hosts))
        return handover(p, *tab, "the page asks the user to sign in");
    return result(*tab, std::move(body));
}

ToolOutput BrowserService::type(ToolContext& ctx, const json& args) {
    auto tab = run_tab(ctx, false);
    if (!tab) return ToolOutput::failure(tab.error().message);
    std::shared_ptr<Process> p;
    {
        const std::lock_guard lock(mx_);
        p = proc_;
    }
    const std::string ref = jstr(args, "ref");
    auto f = facts(p, *tab, ref);
    if (!f) return ToolOutput::failure(f.error().message);
    // No Password Entry By Agent (design D3): refuse, and hand the sign-in to the user.
    if (is_credential_field(*f) || is_sign_in_host(url_host(page_url(*tab)), env_.idp_hosts))
        return handover(p, *tab, "typing into a credential field was refused");
    auto focused = call_on(p, *tab, ref, kFocusFn, json::array({jbool(args, "clear", true)}));
    if (!focused) return ToolOutput::failure(focused.error().message);
    auto& cdp = p->chrome->cdp();
    if (auto r = cdp.call("Input.insertText", {{"text", jstr(args, "text")}}, (*tab)->session, 15s); !r)
        return ToolOutput::failure("Typing failed: " + r.error().message);
    long long dl = 0;
    {
        const std::lock_guard lock(dl_mx_);
        dl = dl_seq_;
    }
    const int before = (*tab)->navigations.load();
    if (jbool(args, "submit")) {
        const json key = {{"key", "Enter"}, {"code", "Enter"}, {"windowsVirtualKeyCode", 13}, {"nativeVirtualKeyCode", 13}};
        json down = key;
        down["type"] = "keyDown";
        down["text"] = "\r";
        json upk = key;
        upk["type"] = "keyUp";
        (void)cdp.call("Input.dispatchKeyEvent", down, (*tab)->session, 10s);
        (void)cdp.call("Input.dispatchKeyEvent", upk, (*tab)->session, 10s);
    }
    json body = settle(p, *tab, before, dl, jbool(args, "submit") ? 15s : 1s);
    body["typed_into"] = f->name.empty() ? ref : f->name;
    return result(*tab, std::move(body));
}

ToolOutput BrowserService::select(ToolContext& ctx, const json& args) {
    auto tab = run_tab(ctx, false);
    if (!tab) return ToolOutput::failure(tab.error().message);
    std::shared_ptr<Process> p;
    {
        const std::lock_guard lock(mx_);
        p = proc_;
    }
    const int before = (*tab)->navigations.load();
    auto r = call_on(p, *tab, jstr(args, "ref"), kSelectFn, json::array({jstr(args, "value")}));
    if (!r) return ToolOutput::failure(r.error().message);
    if (!jbool(*r, "ok")) return ToolOutput::failure(jstr(*r, "error"));
    json body = settle(p, *tab, before, download_mark(), 1s);
    body["selected"] = jstr(*r, "selected");
    return result(*tab, std::move(body));
}

ToolOutput BrowserService::scroll(ToolContext& ctx, const json& args) {
    auto tab = run_tab(ctx, false);
    if (!tab) return ToolOutput::failure(tab.error().message);
    std::shared_ptr<Process> p;
    {
        const std::lock_guard lock(mx_);
        p = proc_;
    }
    auto& cdp = p->chrome->cdp();
    if (const std::string ref = jstr(args, "ref"); !ref.empty()) {
        std::optional<long long> backend;
        {
            const std::lock_guard lock((*tab)->mx);
            backend = (*tab)->refs.resolve(ref);
        }
        if (!backend) return ToolOutput::failure(stale_message(ref));
        auto r = cdp.call("DOM.scrollIntoViewIfNeeded", {{"backendNodeId", *backend}}, (*tab)->session, 10s);
        if (!r) return ToolOutput::failure(is_stale_error(r.error()) ? stale_message(ref) : r.error().message);
        return result(*tab, {{"url", page_url(*tab)}, {"scrolled_to", ref}});
    }
    const double pages = std::clamp(args.value("pages", 1.0), 0.1, 20.0) * (jstr(args, "direction", "down") == "up" ? -1 : 1);
    auto r = cdp.call("Runtime.evaluate",
                      {{"expression", "window.scrollBy(0, " + std::to_string(pages) + " * window.innerHeight * 0.8); "
                                      "[Math.round(window.scrollY), Math.round(document.documentElement.scrollHeight - window.innerHeight)]"},
                       {"returnByValue", true}},
                      (*tab)->session, 10s);
    if (!r) return ToolOutput::failure("Scrolling failed: " + r.error().message);
    const json& v = (*r)["result"]["value"];
    json body = {{"url", page_url(*tab)}};
    if (v.is_array() && v.size() == 2) {
        body["scroll_y"] = v[0];
        body["max_scroll_y"] = v[1];
    }
    return result(*tab, std::move(body));
}

ToolOutput BrowserService::wait(ToolContext& ctx, const json& args) {
    if (const std::string user = jstr(args, "user"); !user.empty()) {
        // The page needs the user (sign-in, MFA, a CAPTCHA): hand over (No Password Entry By Agent).
        auto tab = run_tab(ctx, false);
        if (!tab) return ToolOutput::failure(tab.error().message);
        std::shared_ptr<Process> p;
        {
            const std::lock_guard lock(mx_);
            p = proc_;
        }
        return handover(p, *tab, user);
    }
    auto tab = run_tab(ctx, false);
    if (!tab) return ToolOutput::failure(tab.error().message);
    std::shared_ptr<Process> p;
    {
        const std::lock_guard lock(mx_);
        p = proc_;
    }
    const auto limit = std::chrono::milliseconds(static_cast<long long>(std::clamp(args.value("seconds", 10.0), 0.1, 30.0) * 1000));
    const std::string text = jstr(args, "text");
    const auto start = std::chrono::steady_clock::now();
    if (text.empty()) {
        while (std::chrono::steady_clock::now() - start < limit && !ctx.cancelled()) std::this_thread::sleep_for(50ms);
        return result(*tab, {{"url", page_url(*tab)}, {"waited_seconds", static_cast<double>(limit.count()) / 1000.0}});
    }
    const std::string expr = "(document.body ? document.body.innerText : '').includes(" + json(text).dump() + ")";
    while (std::chrono::steady_clock::now() - start < limit && !ctx.cancelled()) {
        auto r = p->chrome->cdp().call("Runtime.evaluate", {{"expression", expr}, {"returnByValue", true}}, (*tab)->session, 5s);
        if (r && (*r)["result"].value("value", false)) return result(*tab, {{"url", page_url(*tab)}, {"found", text}});
        std::this_thread::sleep_for(200ms);
    }
    return result(*tab, {{"url", page_url(*tab)}, {"found", false}, {"error", "the text did not appear in time"}}, false);
}

ToolOutput BrowserService::tabs(ToolContext& ctx, const json& args) {
    const std::string action = jstr(args, "action", "list");
    if (action == "open") {
        auto e = ensure_browser(ctx.unattended);
        if (!e) return ToolOutput::failure("The agent browser is unavailable: " + e.error().message);
        auto tab = open_tab(*e, ctx.run_id, "about:blank");
        if (!tab) return ToolOutput::failure("The tab could not be opened: " + tab.error().message);
        {
            const std::lock_guard lock(mx_);
            auto& rt = runs_[ctx.run_id];
            if (!rt) rt = std::make_shared<RunTabs>();
            rt->tabs.push_back(*tab);
            rt->active = rt->tabs.size() - 1;
        }
        if (!jstr(args, "url").empty()) return navigate(ctx, args);
        return result(*tab, {{"opened", true}, {"url", "about:blank"}});
    }
    std::shared_ptr<RunTabs> rt;
    std::shared_ptr<Process> p;
    {
        const std::lock_guard lock(mx_);
        p = proc_;
        if (auto it = runs_.find(ctx.run_id); it != runs_.end()) rt = it->second;
    }
    if (!rt || rt->tabs.empty() || !p) return ToolOutput::success("[] (no tabs are open in this Run)");
    const long long index = jint(args, "index", -1);
    std::vector<std::shared_ptr<Tab>> list;
    std::size_t active = 0;
    {
        const std::lock_guard lock(mx_);
        list = rt->tabs;
        active = rt->active;
    }
    if (action == "switch" || action == "close") {
        if (index < 0 || static_cast<std::size_t>(index) >= list.size())
            return ToolOutput::failure("No tab " + std::to_string(index) + "; use browser.tabs with action list.");
        auto tab = list[static_cast<std::size_t>(index)];
        if (action == "switch") {
            {
                const std::lock_guard lock(mx_);
                rt->active = static_cast<std::size_t>(index);
            }
            if (!p->headless) (void)p->chrome->cdp().call("Page.bringToFront", json::object(), tab->session, 5s);
            return result(tab, {{"active", index}, {"url", page_url(tab)}});
        }
        (void)p->chrome->cdp().call("Target.closeTarget", {{"targetId", tab->target_id}}, {}, 10s);
        tab->closed = true;
        {
            const std::lock_guard lock(mx_);
            auto& tabs = rt->tabs;
            tabs.erase(std::remove(tabs.begin(), tabs.end(), tab), tabs.end());
            if (rt->active >= tabs.size()) rt->active = tabs.empty() ? 0 : tabs.size() - 1;
        }
        return ToolOutput::success("Closed tab " + std::to_string(index) + ".");
    }
    json out = json::array();
    for (std::size_t i = 0; i < list.size(); ++i) {
        std::string title;
        if (auto t = p->chrome->cdp().call("Target.getTargetInfo", {{"targetId", list[i]->target_id}}, {}, 5s))
            title = jstr((*t)["targetInfo"], "title");
        out.push_back({{"index", i}, {"url", page_url(list[i])}, {"title", title}, {"active", i == active}});
    }
    return result(list[active], out);
}

ToolOutput BrowserService::back(ToolContext& ctx, const json&) {
    auto tab = run_tab(ctx, false);
    if (!tab) return ToolOutput::failure(tab.error().message);
    std::shared_ptr<Process> p;
    {
        const std::lock_guard lock(mx_);
        p = proc_;
    }
    auto& cdp = p->chrome->cdp();
    auto h = cdp.call("Page.getNavigationHistory", json::object(), (*tab)->session, 10s);
    if (!h) return ToolOutput::failure("The history could not be read: " + h.error().message);
    const long long cur = jint(*h, "currentIndex");
    if (cur <= 0) return ToolOutput::failure("There is no earlier page in this tab.");
    const int before = (*tab)->navigations.load();
    (void)cdp.call("Page.navigateToHistoryEntry", {{"entryId", (*h)["entries"][static_cast<std::size_t>(cur - 1)]["id"]}}, (*tab)->session, 30s);
    return result(*tab, settle(p, *tab, before, download_mark(), 30s));
}

ToolOutput BrowserService::eval(ToolContext& ctx, const json& args) {
    if (!eval_offered()) return ToolOutput::failure("browser.eval is not allowed by policy.");
    auto tab = run_tab(ctx, false);
    if (!tab) return ToolOutput::failure(tab.error().message);
    std::shared_ptr<Process> p;
    {
        const std::lock_guard lock(mx_);
        p = proc_;
    }
    auto r = p->chrome->cdp().call("Runtime.evaluate",
                                   {{"expression", jstr(args, "expression")}, {"returnByValue", true}, {"awaitPromise", true}, {"timeout", 15000}},
                                   (*tab)->session, 30s);
    if (!r) return ToolOutput::failure("The script failed: " + r.error().message);
    if (r->contains("exceptionDetails"))
        return ToolOutput::failure("The script raised: " + jstr((*r)["exceptionDetails"], "text") + " " +
                                   jstr((*r)["exceptionDetails"]["exception"], "description"));
    std::string value = (*r)["result"].contains("value") ? (*r)["result"]["value"].dump(-1, ' ', false, json::error_handler_t::replace)
                                                         : jstr((*r)["result"], "description", "undefined");
    if (value.size() > 16384) value = utf8_truncate(value, 16384) + "...";
    ToolOutput out = ToolOutput::success(value);
    out.provenance = runtime::Provenance{"browser", page_url(*tab), iso8601_utc_now(), false};
    return out;
}

// ---- tool definitions ---------------------------------------------------------------------------------------------

std::vector<runtime::ToolDef> browser_tools(std::shared_ptr<BrowserService> service) {
    using runtime::ToolDef;
    using runtime::ToolResource;
    std::vector<ToolDef> out;
    const auto prop = [](const char* type, const char* description) { return json{{"type", type}, {"description", description}}; };
    const auto ref_prop = prop("string", "Element reference from the latest browser.snapshot, for example e12.");
    const auto add = [&](const char* id, std::string description, json properties, std::vector<std::string> required, Risk max_risk,
                         ToolOutput (BrowserService::*fn)(ToolContext&, const json&), bool eval = false) {
        ToolDef t;
        t.id = id;
        t.description = std::move(description);
        t.parameters = {{"type", "object"}, {"properties", std::move(properties)}};
        if (!required.empty()) t.parameters["required"] = required;
        t.risk = max_risk;
        std::weak_ptr<BrowserService> w = service;
        const std::string tool = id;
        t.risk_in_run = [w, tool](const std::string& run_id, const json& a) {
            auto s = w.lock();
            return s ? s->risk(tool, run_id, a) : Risk::destructive;
        };
        t.offered = [w, eval] {
            auto s = w.lock();
            return s && (eval ? s->eval_offered() : s->offered());
        };
        t.run = [w, fn](ToolContext& ctx, const json& a) {
            auto s = w.lock();
            return s ? ((*s).*fn)(ctx, a) : ToolOutput::failure("The agent browser is shutting down.");
        };
        t.resume = [w](ToolContext& ctx, const json& a, const json& state, const std::string& answer) {
            auto s = w.lock();
            return s ? s->resume_handover(ctx, a, state, answer) : ToolOutput::failure("The agent browser is shutting down.");
        };
        t.provenance = runtime::Provenance{"browser", "", "", false};
        out.push_back(std::move(t));
        return &out.back();
    };

    add("browser.snapshot",
        "Read the current page of the agent browser as an accessibility tree: headings, text, links, buttons and form "
        "fields. Interactive elements carry a reference [ref=eN] for browser.click, browser.type, browser.select and "
        "browser.scroll. References are valid until the page changes; take a new snapshot after a navigation.",
        json::object(), {}, Risk::read, &BrowserService::snapshot);
    add("browser.screenshot",
        "Take a screenshot of the current page and look at it. Use it when the layout matters or the snapshot is not "
        "enough; prefer browser.snapshot for reading text.",
        {{"full_page", prop("boolean", "The whole page instead of the visible part.")}}, {}, Risk::read, &BrowserService::screenshot);
    auto* nav = add("browser.navigate",
                    "Open an http or https URL in the agent browser (Google Chrome with its own profile). Organization sites sign "
                    "in through the organization identity provider; when a page asks for a password or a certificate choice, the "
                    "user takes over. Starts the browser when needed.",
                    {{"url", prop("string", "http:// or https:// URL.")}}, {"url"}, Risk::external, &BrowserService::navigate);
    nav->resources_of = [](const json& a) { return std::vector<ToolResource>{{ToolResource::Kind::url, jstr(a, "url")}}; };
    add("browser.click", "Click an element of the current page by its reference.", {{"ref", ref_prop}}, {"ref"}, Risk::external,
        &BrowserService::click);
    add("browser.type",
        "Type text into a field by its reference. clear (default true) empties the field first; submit presses Enter "
        "afterwards. Never use it for passwords or other credentials: the user signs in themselves.",
        {{"ref", ref_prop},
         {"text", prop("string", "The text to type.")},
         {"clear", prop("boolean", "Empty the field first (default true).")},
         {"submit", prop("boolean", "Press Enter afterwards.")}},
        {"ref", "text"}, Risk::external, &BrowserService::type);
    add("browser.select", "Choose an option of a select element by its value or visible label.",
        {{"ref", ref_prop}, {"value", prop("string", "Option value or label.")}}, {"ref", "value"}, Risk::external,
        &BrowserService::select);
    add("browser.scroll", "Scroll the page by screens (direction up or down, pages default 1), or scroll an element into view.",
        {{"direction", {{"type", "string"}, {"enum", {"down", "up"}}}},
         {"pages", prop("number", "How many screens (default 1).")},
         {"ref", prop("string", "Scroll this element into view instead.")}},
        {}, Risk::read, &BrowserService::scroll);
    add("browser.wait",
        "Wait for text to appear on the page (at most 30 seconds), or for a number of seconds. Set user to a short reason "
        "when the page needs the user (signing in, a second factor, a CAPTCHA): the user is asked to do it and continue.",
        {{"text", prop("string", "Text to wait for.")},
         {"seconds", prop("number", "Seconds to wait, or the limit when waiting for text (default 10, at most 30).")},
         {"user", prop("string", "Why the user has to act in the browser.")}},
        {}, Risk::read, &BrowserService::wait);
    auto* tabs = add("browser.tabs",
                     "List, open, switch or close this task's browser tabs. action: list (default), open (optionally with url), "
                     "switch or close (with index from list).",
                     {{"action", {{"type", "string"}, {"enum", {"list", "open", "switch", "close"}}}},
                      {"url", prop("string", "URL for action open.")},
                      {"index", prop("integer", "Tab index for switch and close.")}},
                     {}, Risk::external, &BrowserService::tabs);
    tabs->resources_of = [](const json& a) {
        std::vector<ToolResource> r;
        if (!jstr(a, "url").empty()) r.push_back({ToolResource::Kind::url, jstr(a, "url")});
        return r;
    };
    add("browser.back", "Go back to the previous page in the current tab.", json::object(), {}, Risk::external, &BrowserService::back);
    add("browser.eval",
        "Run a JavaScript expression in the current page and return its value (allowed only when the administrator "
        "enabled it).",
        {{"expression", prop("string", "JavaScript expression; promises are awaited.")}}, {"expression"}, Risk::destructive,
        &BrowserService::eval, true);
    return out;
}

// ---- egress -------------------------------------------------------------------------------------------------------

policy::RuleVerdict browser_request_verdict(const policy::EffectivePolicy* policy, const std::string& url) {
    const std::string lower = to_lower(url.substr(0, std::min<std::size_t>(url.size(), 32)));
    for (const char* local : {"data:", "blob:", "about:", "chrome-extension:", "devtools:", "chrome:", "chrome-error:"})
        if (starts_with(lower, local)) return {true, "", "not a network request"};
    if (!starts_with(lower, "http:") && !starts_with(lower, "https:") && !starts_with(lower, "ws:") && !starts_with(lower, "wss:"))
        return {false, "browser.scheme", "the agent browser only loads http and https"};
    if (!policy) return {false, "fail-closed", "no valid policy is in force"};
    std::string checked = url;
    if (starts_with(lower, "ws:")) checked = "http:" + url.substr(3);
    else if (starts_with(lower, "wss:")) checked = "https:" + url.substr(4);
    return policy::check_egress_url(checked, policy->strings("network.egress_allow"), policy->strings("network.egress_deny"));
}

}  // namespace wb::browser
