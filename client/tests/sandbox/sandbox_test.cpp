// client/tool-sandbox (add-policy-and-sandbox tasks 4.1-4.8): containment, limits, the restricted token and
// environment, no-network mode, the egress proxy, the enforcement level and output capture. The child processes are
// sandbox_probe.exe, built next to the tests.
#include "base/encoding.h"
#include "platform/crypto.h"
#include "platform/win.h"
#include "sandbox/enforcement.h"
#include "sandbox/proxy.h"
#include "sandbox/sandbox.h"

#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#include <gtest/gtest.h>

#include <atomic>
#include <fstream>
#include <thread>

using namespace wb;
using namespace wb::sandbox;
using namespace std::chrono_literals;

namespace {

std::wstring probe() { return (exe_dir() / L"sandbox_probe.exe").wstring(); }

ProcessSpec probe_spec(const std::wstring& args, Network net = Network::direct) {
    ProcessSpec s;
    s.executable = probe();
    s.arguments = args;
    s.network = net;
    s.tool = "test";
    return s;
}

struct TempDir {
    std::filesystem::path path = std::filesystem::temp_directory_path() / (L"skowork-sbx-" + widen(random_urlsafe(6)));
    TempDir() { std::filesystem::create_directories(path); }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

struct Winsock {
    Winsock() {
        WSADATA w;
        ::WSAStartup(MAKEWORD(2, 2), &w);
    }
    ~Winsock() { ::WSACleanup(); }
};

// A loopback server that answers every connection with a fixed response and records what it received.
class LocalServer {
public:
    explicit LocalServer(std::string response) : response_(std::move(response)) {
        sock_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        ::bind(sock_, reinterpret_cast<sockaddr*>(&a), sizeof a);
        int len = sizeof a;
        ::getsockname(sock_, reinterpret_cast<sockaddr*>(&a), &len);
        port = ntohs(a.sin_port);
        ::listen(sock_, 8);
        thread_ = std::thread([this] {
            for (;;) {
                SOCKET c = ::accept(sock_, nullptr, nullptr);
                if (c == INVALID_SOCKET) return;
                ++accepted;
                char buf[4096];
                const int n = ::recv(c, buf, sizeof buf, 0);
                if (n > 0) {
                    const std::lock_guard lock(mx);
                    received.append(buf, static_cast<size_t>(n));
                }
                ::send(c, response_.data(), static_cast<int>(response_.size()), 0);
                ::shutdown(c, SD_SEND);
                ::closesocket(c);
            }
        });
    }
    ~LocalServer() {
        ::closesocket(sock_);
        thread_.join();
    }
    int port = 0;
    std::atomic<int> accepted{0};
    std::mutex mx;
    std::string received;

private:
    Winsock wsa_;
    std::string response_;
    SOCKET sock_ = INVALID_SOCKET;
    std::thread thread_;
};

// Sends a raw request to the proxy and returns everything it answers.
std::string proxy_exchange(int port, const std::string& request) {
    SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(static_cast<u_short>(port));
    if (::connect(s, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
        ::closesocket(s);
        return "connect failed";
    }
    DWORD to = 10000;
    ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&to), sizeof to);
    ::send(s, request.data(), static_cast<int>(request.size()), 0);
    std::string out;
    char buf[4096];
    for (;;) {
        const int n = ::recv(s, buf, sizeof buf, 0);
        if (n <= 0) break;
        out.append(buf, static_cast<size_t>(n));
        // A tunnel stays open: stop after the server's response arrived through it.
        if (out.find("\r\n\r\n") != std::string::npos && out.find("200 Connection Established") != std::string::npos &&
            out.find("tunnel-ok") != std::string::npos)
            break;
    }
    ::closesocket(s);
    return out;
}

std::string auth(const std::string& cred) {
    return "Proxy-Authorization: Basic " + base64_encode(reinterpret_cast<const std::uint8_t*>(("skowork:" + cred).data()), sizeof("skowork:") - 1 + cred.size()) + "\r\n";
}

bool process_gone(DWORD pid, std::chrono::milliseconds within) {
    unique_handle h(::OpenProcess(SYNCHRONIZE, FALSE, pid));
    if (!h) return true;
    return ::WaitForSingleObject(h.get(), static_cast<DWORD>(within.count())) == WAIT_OBJECT_0;
}

}  // namespace

// ---- 4.1 Containment -------------------------------------------------------------------------------------------------

TEST(SandboxRun, ExitCodeStdinAndWorkingFolder) {
    TempDir t;
    Sandbox sb;
    auto r = sb.run(probe_spec(L"exit 7"));
    ASSERT_TRUE(r) << r.error().message;
    EXPECT_EQ(r->exit_code, 7);
    EXPECT_EQ(r->limit, LimitHit::none);
    ProcessSpec echo = probe_spec(L"echo");
    echo.stdin_data = "hello sandbox";
    r = sb.run(echo);
    ASSERT_TRUE(r);
    EXPECT_EQ(r->out, "hello sandbox");
    ProcessSpec write = probe_spec(L"write here.txt");
    write.working_dir = narrow(t.path.wstring());
    r = sb.run(write);
    ASSERT_TRUE(r);
    EXPECT_TRUE(std::filesystem::exists(t.path / L"here.txt"));
    ProcessSpec missing = probe_spec(L"exit 0");
    missing.working_dir = narrow((t.path / L"nope").wstring());
    EXPECT_FALSE(sb.run(missing));
}

TEST(SandboxRun, KillingTheClientKillsTheWholeTree) {
    TempDir t;
    const std::wstring pidfile = (t.path / L"pid.txt").wstring();
    std::wstring cmd = L"\"" + probe() + L"\" host \"" + pidfile + L"\"";
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    ASSERT_TRUE(::CreateProcessW(probe().c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi));
    unique_handle host(pi.hProcess), thread(pi.hThread);
    ASSERT_EQ(::WaitForSingleObject(host.get(), 20000), WAIT_OBJECT_0);
    DWORD code = 0;
    ::GetExitCodeProcess(host.get(), &code);
    ASSERT_EQ(code, 9u) << "the host did not see the grandchild start";
    DWORD grandchild = 0;
    std::ifstream(std::filesystem::path(pidfile)) >> grandchild;
    ASSERT_NE(grandchild, 0u);
    EXPECT_TRUE(process_gone(grandchild, 5s)) << "pid " << grandchild << " outlived the client";
}

TEST(SandboxRun, BreakawayIsRefused) {
    Sandbox sb;
    auto r = sb.run(probe_spec(L"breakaway"));
    ASSERT_TRUE(r);
    EXPECT_EQ(r->out.rfind("breakaway-refused", 0), 0u) << r->out;
}

// ---- 4.2 Limits ------------------------------------------------------------------------------------------------------

TEST(SandboxLimits, MemoryLimitStopsTheGroup) {
    Sandbox sb;
    ProcessSpec s = probe_spec(L"alloc 1024");
    s.limits.memory_mb = 64;
    auto r = sb.run(s);
    ASSERT_TRUE(r);
    EXPECT_EQ(r->limit, LimitHit::memory) << r->out;
    EXPECT_NE(describe_limit(r->limit, s.limits, effective_wall(s)).find("memory limit of 64 MB"), std::string::npos);
}

TEST(SandboxLimits, ProcessLimitStopsTheGroup) {
    Sandbox sb;
    ProcessSpec s = probe_spec(L"spawn 10");
    s.limits.processes = 4;
    auto r = sb.run(s);
    ASSERT_TRUE(r);
    EXPECT_EQ(r->limit, LimitHit::processes) << r->out;
}

TEST(SandboxLimits, SmallerTimeoutWins) {
    ProcessSpec s = probe_spec(L"sleep 60000");
    s.limits.wall_minutes = 15;
    s.timeout = 30min;
    EXPECT_EQ(effective_wall(s), std::chrono::milliseconds(15min));
    s.timeout = 400ms;
    EXPECT_EQ(effective_wall(s), 400ms);
    Sandbox sb;
    const auto t0 = std::chrono::steady_clock::now();
    auto r = sb.run(s);
    ASSERT_TRUE(r);
    EXPECT_EQ(r->limit, LimitHit::time);
    EXPECT_LT(std::chrono::steady_clock::now() - t0, 10s);
    EXPECT_NE(describe_limit(LimitHit::time, s.limits, 15min).find("15 minutes"), std::string::npos);
}

TEST(SandboxLimits, CancellationStopsTheGroup) {
    Sandbox sb;
    const auto t0 = std::chrono::steady_clock::now();
    auto r = sb.run(probe_spec(L"sleep 60000"), [t0] { return std::chrono::steady_clock::now() - t0 > 300ms; });
    ASSERT_TRUE(r);
    EXPECT_EQ(r->limit, LimitHit::cancelled);
}

TEST(SandboxLimits, FromPolicy) {
    policy::PolicyBundle b;
    b.ver = 1;
    b.exp = 4102444800;
    b.policy = {{"tools", {{"shell", {{"mode", "sandboxed"}, {"limits", {{"memory_mb", 2048}, {"wall_minutes", 5}}}}}}}};
    const policy::EffectivePolicy p(b);
    const Limits l = limits_for(&p, "shell");
    EXPECT_EQ(l.memory_mb, 2048);
    EXPECT_EQ(l.wall_minutes, 5);
    EXPECT_EQ(l.cpu_percent, 50);
    EXPECT_EQ(l.processes, 32);
    EXPECT_EQ(l.output_kb, 1024);
    EXPECT_EQ(limits_for(nullptr, "shell").memory_mb, 1024);
    EXPECT_EQ(shell_mode(&p), ShellMode::sandboxed);
    EXPECT_EQ(shell_mode(nullptr), ShellMode::off);
    b.policy = json::object();
    const policy::EffectivePolicy unset(b);
    EXPECT_EQ(shell_mode(&unset), ShellMode::off);
}

// ---- 4.4 Shell modes -------------------------------------------------------------------------------------------------

TEST(SandboxShell, ModesDecideTokenNetworkAndWorkingFolder) {
    policy::PathRules rules;
    rules.workspace = "C:\\Data\\workspace";
    rules.data_root = "C:\\Data";
    rules.write = {"D:\\DEV\\**"};
    policy::PolicyBundle b;
    b.ver = 1;
    b.exp = 4102444800;
    const auto with_mode = [&](const char* mode) {
        b.policy = {{"tools", {{"shell", {{"mode", mode}, {"limits", {{"wall_minutes", 3}}}}}}}};
        return policy::EffectivePolicy(b);
    };
    const auto off = with_mode("off");
    auto r = shell_process(&off, rules, "C:\\Data\\workspace");
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, "tool_unavailable");
    EXPECT_FALSE(shell_process(nullptr, rules, "C:\\Data\\workspace"));

    const auto sandboxed = with_mode("sandboxed");
    r = shell_process(&sandboxed, rules, "D:\\DEV\\proj");
    ASSERT_TRUE(r) << r.error().message;
    EXPECT_EQ(r->privileges, Privileges::restricted);
    EXPECT_EQ(r->network, Network::proxy);
    EXPECT_EQ(r->limits.wall_minutes, 3);
    EXPECT_TRUE(shell_process(&sandboxed, rules, "C:\\Data\\workspace\\sub"));
    for (const char* outside : {"C:\\Windows", "C:\\Data", "E:\\other"}) {
        auto denied = shell_process(&sandboxed, rules, outside);
        ASSERT_FALSE(denied) << outside;
        EXPECT_EQ(denied.error().code, "policy_denied") << outside;
    }

    const auto allow = with_mode("allow");
    r = shell_process(&allow, rules, "C:\\Windows");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->privileges, Privileges::user);
    EXPECT_EQ(r->network, Network::direct);
}

// ---- 4.3 Token and environment ---------------------------------------------------------------------------------------

TEST(SandboxPrivileges, RestrictedTokenHasNoAdminAndNoPrivileges) {
    Sandbox sb;
    ProcessSpec s = probe_spec(L"token");
    s.privileges = Privileges::restricted;
    auto r = sb.run(s);
    ASSERT_TRUE(r) << r.error().message;
    EXPECT_TRUE(r->out.find("admins=absent") != std::string::npos || r->out.find("admins=deny-only") != std::string::npos) << r->out;
    EXPECT_NE(r->out.find("privileges=1\n"), std::string::npos) << r->out;  // SeChangeNotifyPrivilege only
}

TEST(SandboxPrivileges, EnvironmentCarriesNoSecrets) {
    ::SetEnvironmentVariableW(L"SKOWORK_TEST_ACCESS_TOKEN", L"eyJ-secret-value");
    ::SetEnvironmentVariableW(L"GITHUB_TOKEN", L"ghp-secret-value");
    Sandbox sb;
    ProcessSpec s = probe_spec(L"env");
    s.privileges = Privileges::restricted;
    s.env = {{"SKOWORK_WORKSPACE", "C:\\w"}};
    auto r = sb.run(s);
    ::SetEnvironmentVariableW(L"SKOWORK_TEST_ACCESS_TOKEN", nullptr);
    ::SetEnvironmentVariableW(L"GITHUB_TOKEN", nullptr);
    ASSERT_TRUE(r);
    EXPECT_EQ(r->out.find("secret-value"), std::string::npos) << r->out;
    EXPECT_EQ(r->out.find("TOKEN"), std::string::npos);
    EXPECT_NE(r->out.find("SKOWORK_WORKSPACE=C:\\w"), std::string::npos);
    EXPECT_NE(r->out.find("SystemRoot="), std::string::npos);
}

// ---- 4.6 Egress proxy ------------------------------------------------------------------------------------------------

namespace {

struct ProxyFixture {
    std::mutex mx;
    std::vector<std::pair<EgressProxy::Owner, std::string>> denials;
    std::vector<std::string> rules;  // the rule of each denial
    EgressProxy proxy;
    explicit ProxyFixture(bool loopback = true)
        : proxy(
              [](const std::string& host) {
                  return policy::check_egress_host(host, {"127.0.0.1", "*.corp.local"}, {"secret.corp.local"});
              },
              [this](const EgressProxy::Owner& o, const std::string& host, const policy::RuleVerdict& v) {
                  const std::lock_guard lock(mx);
                  denials.emplace_back(o, host);
                  rules.push_back(v.rule);
              },
              loopback) {}
};

}  // namespace

TEST(SandboxProxy, CredentialRequired) {
    ProxyFixture f;
    LocalServer server("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok");
    auto session = f.proxy.open({"run-1", "call-1", "shell.run"});
    ASSERT_TRUE(session) << session.error().message;
    const int port = (*session)->port();
    const std::string target = "127.0.0.1:" + std::to_string(server.port);
    EXPECT_NE(proxy_exchange(port, "CONNECT " + target + " HTTP/1.1\r\nHost: " + target + "\r\n\r\n").find(" 407 "), std::string::npos);
    EXPECT_NE(proxy_exchange(port, "CONNECT " + target + " HTTP/1.1\r\n" + auth("wrong-credential-xxxxx") + "\r\n").find(" 407 "),
              std::string::npos);
    EXPECT_EQ(server.accepted.load(), 0);
}

TEST(SandboxProxy, AllowedHostsAreForwardedAndDeniedHostsLogged) {
    ProxyFixture f;
    LocalServer server("HTTP/1.1 200 OK\r\nContent-Length: 9\r\n\r\ntunnel-ok");
    auto session = f.proxy.open({"run-1", "call-1", "shell.run"});
    ASSERT_TRUE(session);
    const int port = (*session)->port();
    const std::string a = auth((*session)->credential());
    const std::string target = "127.0.0.1:" + std::to_string(server.port);

    // CONNECT to an allowed host, then a request through the tunnel.
    const std::string tunnel =
        proxy_exchange(port, "CONNECT " + target + " HTTP/1.1\r\n" + a + "\r\nGET /t HTTP/1.1\r\nHost: " + target + "\r\n\r\n");
    EXPECT_NE(tunnel.find("200 Connection Established"), std::string::npos) << tunnel;
    EXPECT_NE(tunnel.find("tunnel-ok"), std::string::npos) << tunnel;

    // Plain HTTP through the proxy: origin-form upstream, no proxy credentials passed on.
    const std::string fwd = proxy_exchange(port, "GET http://" + target + "/x?y=1 HTTP/1.1\r\nHost: " + target + "\r\n" + a + "\r\n");
    EXPECT_NE(fwd.find("tunnel-ok"), std::string::npos) << fwd;
    {
        const std::lock_guard lock(server.mx);
        EXPECT_NE(server.received.find("GET /x?y=1 HTTP/1.1"), std::string::npos) << server.received;
        EXPECT_EQ(server.received.find("Proxy-Authorization"), std::string::npos);
    }

    // Denied: not in the allow list, and in the deny list.
    EXPECT_NE(proxy_exchange(port, "CONNECT example.com:443 HTTP/1.1\r\n" + a + "\r\n").find(" 403 "), std::string::npos);
    EXPECT_NE(proxy_exchange(port, "CONNECT secret.corp.local:443 HTTP/1.1\r\n" + a + "\r\n").find(" 403 "), std::string::npos);
    EXPECT_NE(proxy_exchange(port, "GET https://example.com/ HTTP/1.1\r\n" + a + "\r\n").find(" 400 "), std::string::npos);
    // A host that is not a valid name, though it ends with an allowed domain (Egress Host Validation).
    EXPECT_NE(proxy_exchange(port, "CONNECT evil.example%2f.corp.local:443 HTTP/1.1\r\n" + a + "\r\n").find(" 403 "), std::string::npos);
    const std::lock_guard lock(f.mx);
    ASSERT_EQ(f.denials.size(), 3u);
    EXPECT_EQ(f.denials[0].first.call_id, "call-1");
    EXPECT_EQ(f.denials[0].second, "example.com");
    EXPECT_EQ(f.denials[1].second, "secret.corp.local");
    EXPECT_EQ(f.rules[2], "network");
}

TEST(SandboxProxy, NeverReachesLocalAddresses) {
    ProxyFixture f(false);
    LocalServer server("HTTP/1.1 200 OK\r\n\r\n");
    auto session = f.proxy.open({"run-1", "call-2", "shell.run"});
    ASSERT_TRUE(session);
    const std::string target = "127.0.0.1:" + std::to_string(server.port);
    EXPECT_NE(proxy_exchange((*session)->port(), "CONNECT " + target + " HTTP/1.1\r\n" + auth((*session)->credential()) + "\r\n").find(" 403 "),
              std::string::npos);
    EXPECT_EQ(server.accepted.load(), 0);
}

TEST(SandboxProxy, RunsOnlyWhileSessionsAreOpen) {
    ProxyFixture f;
    EXPECT_FALSE(f.proxy.running());
    auto s1 = f.proxy.open({});
    auto s2 = f.proxy.open({});
    ASSERT_TRUE(s1 && s2);
    EXPECT_TRUE(f.proxy.running());
    const int port = (*s1)->port();
    EXPECT_NE((*s1)->credential(), (*s2)->credential());
    s1->reset();
    EXPECT_TRUE(f.proxy.running());
    s2->reset();
    EXPECT_FALSE(f.proxy.running());
    EXPECT_EQ(proxy_exchange(port, "CONNECT a:1 HTTP/1.1\r\n\r\n"), "connect failed");
}

TEST(SandboxProxy, SandboxedProcessGetsItsOwnCredential) {
    ProxyFixture f;
    Sandbox sb(&f.proxy);
    ProcessSpec s = probe_spec(L"env", Network::proxy);
    s.privileges = Privileges::restricted;
    auto r = sb.run(s);
    ASSERT_TRUE(r) << r.error().message;
    EXPECT_NE(r->out.find("HTTPS_PROXY=http://skowork:"), std::string::npos) << r->out;
    EXPECT_NE(r->out.find("@127.0.0.1:"), std::string::npos);
    EXPECT_FALSE(f.proxy.running());  // stopped with the process
    Sandbox none;
    EXPECT_FALSE(none.run(probe_spec(L"env", Network::proxy)));  // no proxy, no network-by-proxy process
}

// ---- 4.5 No-network mode ---------------------------------------------------------------------------------------------

TEST(SandboxNoNetwork, EveryDestinationIsRefusedAndLogged) {
    ProxyFixture f;
    LocalServer server("HTTP/1.1 200 OK\r\nContent-Length: 9\r\n\r\ntunnel-ok");
    const std::string target = "127.0.0.1:" + std::to_string(server.port);  // allowed by the fixture's egress rules
    auto normal = f.proxy.open({"run-1", "call-1", "shell.run"});
    auto offline = f.proxy.open({"run-1", "call-2", "skill.script", true});
    ASSERT_TRUE(normal && offline);
    const int port = (*offline)->port();

    // Control: an ordinary credential reaches the allowed host.
    EXPECT_NE(proxy_exchange(port, "CONNECT " + target + " HTTP/1.1\r\n" + auth((*normal)->credential()) + "\r\nGET / HTTP/1.1\r\nHost: " +
                                       target + "\r\n\r\n")
                  .find("tunnel-ok"),
              std::string::npos);
    const int before = server.accepted.load();

    const std::string a = auth((*offline)->credential());
    EXPECT_NE(proxy_exchange(port, "CONNECT " + target + " HTTP/1.1\r\n" + a + "\r\n").find(" 403 "), std::string::npos);
    EXPECT_NE(proxy_exchange(port, "GET http://" + target + "/ HTTP/1.1\r\nHost: " + target + "\r\n" + a + "\r\n").find(" 403 "),
              std::string::npos);
    EXPECT_NE(proxy_exchange(port, "CONNECT example.com:443 HTTP/1.1\r\n" + a + "\r\n").find(" 403 "), std::string::npos);
    EXPECT_EQ(server.accepted.load(), before);
    const std::lock_guard lock(f.mx);
    ASSERT_EQ(f.denials.size(), 3u);
    for (const auto& [owner, host] : f.denials) EXPECT_EQ(owner.call_id, "call-2") << host;
    ASSERT_EQ(f.rules.size(), 3u);
    for (const auto& rule : f.rules) EXPECT_EQ(rule, "no-network");
}

TEST(SandboxNoNetwork, ProcessRunsRestrictedWithTheProxy) {
    ProxyFixture f;
    Sandbox sb(&f.proxy);
    ProcessSpec s = probe_spec(L"env", Network::none);
    s.privileges = Privileges::restricted;
    auto r = sb.run(s);
    ASSERT_TRUE(r) << r.error().message;
    EXPECT_NE(r->out.find("HTTPS_PROXY=http://skowork:"), std::string::npos) << r->out;
    EXPECT_NE(r->out.find("NO_PROXY=\n"), std::string::npos) << r->out;
    EXPECT_NE(r->out.find("NODE_USE_ENV_PROXY=1"), std::string::npos) << r->out;
    s.arguments = L"token";
    r = sb.run(s);
    ASSERT_TRUE(r) << r.error().message;
    EXPECT_NE(r->out.find("privileges=1\n"), std::string::npos) << r->out;
    EXPECT_FALSE(f.proxy.running());
    Sandbox without_proxy;
    EXPECT_FALSE(without_proxy.run(probe_spec(L"env", Network::none)));  // never started without its proxy
}

// ---- 4.7 Enforcement level -------------------------------------------------------------------------------------------

TEST(SandboxEnforcement, RuleDetection) {
    EXPECT_TRUE(is_enforcing_rule(L"v2.30|Action=Block|Active=TRUE|Dir=Out|Protocol=6|EmbedCtxt=Skowork Child Processes|Name=x|"));
    EXPECT_FALSE(is_enforcing_rule(L"v2.30|Action=Block|Active=FALSE|Dir=Out|EmbedCtxt=Skowork Child Processes|"));
    EXPECT_FALSE(is_enforcing_rule(L"v2.30|Action=Allow|Active=TRUE|Dir=Out|EmbedCtxt=Skowork Child Processes|"));
    EXPECT_FALSE(is_enforcing_rule(L"v2.30|Action=Block|Active=TRUE|Dir=In|EmbedCtxt=Skowork Child Processes|"));
    EXPECT_FALSE(is_enforcing_rule(L"v2.30|Action=Block|Active=TRUE|Dir=Out|EmbedCtxt=Skowork Child Processes 2|"));
    // This workstation has no such profile.
    EXPECT_EQ(detect_network_enforcement(), "proxy-only");
}

// ---- 4.8 Output capture ----------------------------------------------------------------------------------------------

TEST(SandboxOutput, HundredMegabytesAreCappedWithoutBlocking) {
    Sandbox sb;
    ProcessSpec s = probe_spec(L"print 100");
    auto r = sb.run(s);
    ASSERT_TRUE(r);
    EXPECT_EQ(r->limit, LimitHit::none);
    EXPECT_EQ(r->exit_code, 0);
    EXPECT_EQ(r->out.size(), 1024u * 1024u);
    EXPECT_TRUE(r->out_truncated);
    EXPECT_EQ(r->out_bytes, 100LL * 1024 * 1024);
    EXPECT_EQ(r->err, "done\n");
    EXPECT_FALSE(r->err_truncated);
}

TEST(SandboxOutput, DecodingIsSafe) {
    Sandbox sb;
    auto r = sb.run(probe_spec(L"oem"));
    ASSERT_TRUE(r);
    const UINT oem = ::GetOEMCP();
    if (oem == 437 || oem == 850 || oem == 852) EXPECT_EQ(r->out, "Gr\xC3\xBC\xC3\x9F" "e\n");
    EXPECT_EQ(decode_output("plain"), "plain");
    EXPECT_EQ(decode_output("Gr\xC3\xBC\xC3"), "Gr\xC3\xBC");  // a sequence cut by the capture limit
    const std::string utf16("h\0i\0\xE1\0", 6);
    EXPECT_EQ(decode_output(utf16), "hi\xC3\xA1");
    EXPECT_EQ(decode_output(std::string("\xFF\xFE" "a\0", 4)), "a");
}
