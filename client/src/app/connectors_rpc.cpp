#include "app/connectors_rpc.h"

namespace wb {

void register_connectors_rpc(RpcRegistry& rpc, std::function<connectors::Request()> request) {
    const auto req = [request = std::move(request)]() -> connectors::Request { return request ? request() : connectors::Request{}; };
    rpc.add("connectors.list", RpcExposure::ui, [req](const json&) -> Result<json> { return connectors::list_connectors(req()); });
    // The token is read from the parameters only; the answer carries whether a token is set, never the token.
    rpc.add("connectors.token.set", RpcExposure::ui, [req](const json& p) -> Result<json> {
        return connectors::set_token(req(), jstr(p, "id"), jstr(p, "token"));
    });
    rpc.add("connectors.token.remove", RpcExposure::ui, [req](const json& p) -> Result<json> {
        return connectors::remove_token(req(), jstr(p, "id"));
    });
}

}  // namespace wb
