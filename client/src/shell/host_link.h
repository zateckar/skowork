// How the native window reaches the host (add-native-ui design D5): the same RPC methods the web UI called, run
// in-process. A call runs on a background executor (handlers may block on the database or the network) and its
// reply is delivered on the window's thread. Portable, so a test can drive the screens with a scripted host.
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <utility>

#include "base/json.h"
#include "base/result.h"

namespace wb::shell {

class HostLink {
public:
    using Reply = std::function<void(Result<json>)>;
    virtual ~HostLink() = default;
    // Calls a host method; `reply` runs later on the window's thread (never inside call()).
    virtual void call(const std::string& method, const json& params, Reply reply) = 0;
};

// A HostLink over a method lookup (the RpcRegistry) and two executors.
class AsyncLink : public HostLink {
public:
    using Handler = std::function<Result<json>(const json&)>;
    using Find = std::function<Handler(const std::string& method)>;  // empty: unknown method
    using Post = std::function<void(std::function<void()>)>;

    AsyncLink(Find find, Post background, Post to_ui)
        : find_(std::move(find)), background_(std::move(background)), to_ui_(std::move(to_ui)) {}

    void call(const std::string& method, const json& params, Reply reply) override {
        Handler h = find_(method);
        if (!h) {
            to_ui_([reply = std::move(reply), method] { reply(make_error("method_not_found", "unknown method " + method)); });
            return;
        }
        background_([h = std::move(h), params, reply = std::move(reply), to_ui = to_ui_]() mutable {
            auto result = std::make_shared<Result<json>>(h(params));
            to_ui([reply = std::move(reply), result] { reply(std::move(*result)); });
        });
    }

private:
    Find find_;
    Post background_, to_ui_;
};

}  // namespace wb::shell
