// dev/graph-mock for the client/mail-calendar tests: the mock as a node process on free ports, a browser that follows
// the authorize redirect into the loopback listener, an encrypted app.db, and the development-build policy that points
// the client at the mock. Each test file gets its own copy (anonymous namespace).
#pragma once

#include "base/time.h"
#include "identity/token_manager.h"
#include "mail/mail_config.h"
#include "platform/crypto.h"
#include "platform/http.h"
#include "platform/win.h"
#include "storage/app_store.h"
#include "storage/data_root.h"
#include "storage/keystore.h"

#include <gtest/gtest.h>

#include <windows.h>

#include <atomic>
#include <filesystem>
#include <memory>
#include <string>

namespace {

inline std::filesystem::path graph_mock_node() {
    wchar_t buf[MAX_PATH];
    if (::SearchPathW(nullptr, L"node.exe", nullptr, MAX_PATH, buf, nullptr) == 0) return {};
    return buf;
}

inline std::filesystem::path graph_mock_dir() { return std::filesystem::path(SKOWORK_GRAPH_MOCK_DIR); }

// node is on PATH and `npm ci` ran in dev/graph-mock.
inline bool graph_mock_ready() {
    std::error_code ec;
    return !graph_mock_node().empty() && std::filesystem::exists(graph_mock_dir() / "node_modules" / "express", ec) &&
           std::filesystem::exists(graph_mock_dir() / "node_modules" / "jose", ec);
}

// dev/graph-mock on free ports with the fixture times relative to now; terminated on destruction or kill().
struct GraphMockProcess {
    PROCESS_INFORMATION pi{};
    HANDLE out = nullptr;
    std::string graph, control;
    long long anchor = wb::unix_millis_now();

    GraphMockProcess() {
        SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
        HANDLE w = nullptr;
        ::CreatePipe(&out, &w, &sa, 0);
        ::SetHandleInformation(out, HANDLE_FLAG_INHERIT, 0);
        STARTUPINFOW si{};
        si.cb = sizeof si;
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = w;
        si.hStdError = w;
        si.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);
        const std::filesystem::path node = graph_mock_node();
        const std::string at = wb::iso8601_utc(wb::sys_clock::time_point(std::chrono::milliseconds(anchor)));
        std::wstring cmd = L"\"" + node.wstring() + L"\" \"" + (graph_mock_dir() / "src" / "main.ts").wstring() +
                           L"\" --port 0 --control-port 0 --anchor " + wb::widen(at);
        const std::wstring cwd = graph_mock_dir().wstring();
        ::CreateProcessW(node.c_str(), cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, cwd.c_str(), &si, &pi);
        ::CloseHandle(w);
        std::string line;
        char c = 0;
        DWORD n = 0;
        while (::ReadFile(out, &c, 1, &n, nullptr) && n == 1 && c != '\n') line.push_back(c);
        if (const auto j = wb::parse_json(line)) {
            graph = wb::jstr(*j, "graph");
            control = wb::jstr(*j, "control");
        }
    }
    ~GraphMockProcess() { kill(); }
    GraphMockProcess(const GraphMockProcess&) = delete;
    GraphMockProcess& operator=(const GraphMockProcess&) = delete;

    void kill() {
        if (pi.hProcess) {
            ::TerminateProcess(pi.hProcess, 0);
            ::WaitForSingleObject(pi.hProcess, 5000);
            ::CloseHandle(pi.hProcess);
            ::CloseHandle(pi.hThread);
            pi = {};
        }
        if (out) ::CloseHandle(out);
        out = nullptr;
    }

    // The control API; the parsed body (null when empty).
    wb::json ctl(const std::string& method, const std::string& path, const wb::json& body = nullptr) {
        wb::HttpRequest r;
        r.method = method;
        r.url = control + "/_mock" + path;
        if (!body.is_null()) {
            r.headers = {{"Content-Type", "application/json"}};
            r.body = body.dump();
        }
        auto resp = wb::http().send(r);
        EXPECT_TRUE(resp) << path;
        if (!resp) return nullptr;
        EXPECT_LT(resp->status, 400) << method << " " << path << ": " << resp->body;
        if (resp->body.empty()) return nullptr;
        auto j = wb::parse_json(resp->body);
        return j ? *j : wb::json(nullptr);
    }

    // The development-build policy that points the client at this mock.
    [[nodiscard]] wb::json policy() const {
        return {{"mail.auth", "mock"},
                {"mail.graph_base_url", graph + "/v1.0"},
                {"mail.oauth.authority", graph},
                {"mail.oauth.tenant_id", "mock-tenant"},
                {"mail.oauth.client_id", "skowork-dev"}};
    }
};

// The browser of a test: follows the authorize redirect into the loopback listener, as the system browser would.
class FollowingBrowser final : public wb::BrowserLauncher {
public:
    wb::Status open(const std::string& url) override {
        ++opened;
        last_url = url;
        if (!follow) return {};
        wb::HttpRequest r;
        r.url = url;
        r.follow_redirects = false;
        auto a = wb::http().send(r);
        if (!a) return a.error();
        const std::string loc = a->header("location");
        if (loc.empty()) return wb::make_error("no_redirect", a->body);
        landing = loc;
        wb::HttpRequest b;
        b.url = loc;
        b.follow_redirects = false;
        auto done = wb::http().send(b);
        if (!done) return done.error();
        landing_status = done->status;
        return {};
    }
    // The redirect URI of the last sign-in (http://localhost:<port>/).
    [[nodiscard]] std::string redirect() const { return landing.substr(0, landing.find('?')); }

    std::atomic<int> opened{0};
    std::atomic<bool> follow{true};
    std::string last_url, landing;
    int landing_status = 0;
};

// An encrypted app.db with every migration, in its own temporary data root. open() again is a new process.
struct MailDb {
    MailDb() {
        auto root = wb::DataRoot::open(path);
        EXPECT_TRUE(root);
        wb::KeyStore keys(root->keys());
        EXPECT_TRUE(keys.load());
        auto k = keys.get_or_create("app.db");
        EXPECT_TRUE(k);
        key = std::make_unique<wb::db_key>(*k);
        file = root->file("app.db");
        open();
    }
    ~MailDb() {
        store.reset();
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
    MailDb(const MailDb&) = delete;
    MailDb& operator=(const MailDb&) = delete;

    void open() {
        store.reset();
        wb::MigrationOutcome o;
        auto opened = wb::open_and_migrate(file, *key, wb::app_migrations(), o);
        EXPECT_TRUE(opened);
        store = std::make_unique<wb::AppStore>(std::move(*opened));
    }
    std::filesystem::path path = std::filesystem::path(wb::local_appdata()) / L"skowork-test" / wb::widen(wb::random_urlsafe(6));
    std::unique_ptr<wb::db_key> key;
    std::filesystem::path file;
    std::unique_ptr<wb::AppStore> store;
};

// A policy lookup over a flat JSON object of keys.
inline wb::mail::PolicyLookup policy_from(const wb::json* p) {
    return [p](const std::string& k) -> std::optional<wb::json> {
        if (p->contains(k)) return (*p)[k];
        return std::nullopt;
    };
}

}  // namespace
