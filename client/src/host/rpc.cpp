#include "host/rpc.h"

namespace wb {

void RpcRegistry::add(const std::string& method, RpcExposure exposure, Handler handler) {
    const std::lock_guard lock(mx_);
    methods_[method] = Entry{exposure, std::move(handler)};
}

const RpcRegistry::Handler* RpcRegistry::find(const std::string& method, bool from_pipe) const {
    const std::lock_guard lock(mx_);
    const auto it = methods_.find(method);
    if (it == methods_.end()) return nullptr;
    if (from_pipe && it->second.exposure != RpcExposure::ui_and_pipe) return nullptr;
    return &it->second.handler;
}

std::string RpcRegistry::handle_pipe_request(const std::string& request) const {
    auto req = parse_json(request);
    json id = nullptr;
    auto error = [&](int code, const std::string& message) {
        return json{{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", message}}}}.dump();
    };
    if (!req || !req->is_object()) return error(-32700, "parse error");
    if (const auto it = req->find("id"); it != req->end()) id = *it;
    const std::string method = jstr(*req, "method");
    if (method.empty()) return error(-32600, "invalid request");
    const Handler* h = find(method, true);
    if (!h) return error(-32601, "method not found");
    json params = json::object();
    if (const auto it = req->find("params"); it != req->end()) params = *it;
    auto result = (*h)(params);
    if (!result) return json{{"jsonrpc", "2.0"}, {"id", id},
                             {"error", {{"code", -32000}, {"message", result.error().message},
                                        {"data", {{"code", result.error().code}}}}}}.dump();
    return json{{"jsonrpc", "2.0"}, {"id", id}, {"result", *result}}.dump();
}

}  // namespace wb
