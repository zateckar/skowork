// One method registry for the native window and the local API (named pipe, JSON-RPC 2.0).
// Every capability registers its methods here; handlers run on the thread pool and may block.
#pragma once

#include "base/json.h"
#include "base/result.h"

#include <functional>
#include <map>
#include <mutex>
#include <string>

namespace wb {

// pipe: the local API only, never the window (add-evaluation-export design D1).
enum class RpcExposure { ui, ui_and_pipe, pipe };

class RpcRegistry {
public:
    using Handler = std::function<Result<json>(const json& params)>;

    void add(const std::string& method, RpcExposure exposure, Handler handler);
    // Returns nullptr when unknown, not exposed on the pipe (from_pipe) or pipe-only (from the window).
    [[nodiscard]] const Handler* find(const std::string& method, bool from_pipe) const;

    // JSON-RPC 2.0 over the pipe: {"jsonrpc":"2.0","id":..,"method":..,"params":..}.
    std::string handle_pipe_request(const std::string& request) const;

private:
    struct Entry {
        RpcExposure exposure;
        Handler handler;
    };
    mutable std::mutex mx_;
    std::map<std::string, Entry> methods_;
};

}  // namespace wb
