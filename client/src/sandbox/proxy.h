// The egress proxy for sandboxed child processes (client/tool-sandbox Egress Proxy For Child Processes; design D6): an
// HTTP CONNECT and forward proxy on 127.0.0.1, running only while sessions are open, accepting only requests with a
// per-process credential, and checking every destination host against the egress rules before connecting.
#pragma once

#include "base/result.h"
#include "policy/core.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <optional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace wb::sandbox {

class EgressProxy {
public:
    struct Owner {
        std::string run_id, call_id, tool;
        // No-network mode (client/tool-sandbox): every destination is refused for this credential, whatever the rules.
        bool no_network = false;
    };
    // The egress check for a host (policy.network.egress_allow / egress_deny); no policy means deny.
    using HostCheck = std::function<policy::RuleVerdict(const std::string& host)>;
    // Called for every refused destination, for the approval log.
    using DenialSink = std::function<void(const Owner&, const std::string& host, const policy::RuleVerdict&)>;

    // `loopback_upstream` lets tests reach a local server through the proxy; the client never sets it.
    EgressProxy(HostCheck check, DenialSink on_denied, bool loopback_upstream = false);
    ~EgressProxy();
    EgressProxy(const EgressProxy&) = delete;
    EgressProxy& operator=(const EgressProxy&) = delete;

    // A credential for one sandboxed process. The proxy listens while at least one session is open.
    class Session {
    public:
        ~Session();
        [[nodiscard]] int port() const { return port_; }
        [[nodiscard]] const std::string& credential() const { return credential_; }
        // http://skowork:<credential>@127.0.0.1:<port>
        [[nodiscard]] std::string proxy_url() const;

    private:
        friend class EgressProxy;
        Session(EgressProxy& p, long long id, int port, std::string cred) : proxy_(p), id_(id), port_(port), credential_(std::move(cred)) {}
        EgressProxy& proxy_;
        long long id_;
        int port_;
        std::string credential_;
    };

    Result<std::unique_ptr<Session>> open(Owner owner);

    [[nodiscard]] bool running() const;
    [[nodiscard]] int active_connections() const { return connections_.load(); }

    static constexpr int kMaxConnections = 128;

private:
    struct Entry {
        std::string credential;
        Owner owner;
    };
    void close_session(long long id);
    Status start_locked();
    void stop();
    void accept_loop(std::uintptr_t listen_socket);
    void serve(std::uintptr_t client);
    // The owner whose credential the Proxy-Authorization header carries (constant-time comparison).
    std::optional<Owner> authenticate(const std::string& header_value);

    HostCheck check_;
    DenialSink on_denied_;
    bool loopback_upstream_ = false;
    mutable std::mutex mx_;
    std::map<long long, Entry> sessions_;
    long long next_id_ = 1;
    std::uintptr_t listen_ = ~std::uintptr_t{0};
    int port_ = 0;
    std::thread acceptor_;
    std::atomic<int> connections_{0};
    std::vector<std::uintptr_t> open_sockets_;  // shut down on stop so that connection threads end
    std::condition_variable idle_;              // connections_ reached zero
};

// The host and port of an authority ("host:port", "[v6]:port"); default port when absent.
bool split_authority(const std::string& authority, int default_port, std::string& host, int& port);

}  // namespace wb::sandbox
