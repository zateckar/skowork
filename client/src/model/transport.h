// The seams of model access: the HTTP transport and the token source. Production uses WinHTTP and the identity
// token manager; tests inject a scripted gateway.
#pragma once

#include "base/result.h"
#include "identity/token_manager.h"
#include "platform/http.h"

#include <string>

namespace wb::model {

class ITransport {
public:
    virtual ~ITransport() = default;
    virtual Result<HttpResponse> send(const HttpRequest& request) = 0;
};

class WinHttpTransport final : public ITransport {
public:
    Result<HttpResponse> send(const HttpRequest& request) override { return http().send(request); }
};

// Access tokens for the gateway (client/identity: refreshed when fewer than 60 s remain, one refresh-and-retry on 401).
class IGatewayAuth {
public:
    virtual ~IGatewayAuth() = default;
    virtual Result<std::string> access_token() = 0;
    virtual Result<std::string> refresh_after_unauthorized(const std::string& rejected) = 0;
    virtual void session_rejected(const std::string& reason) = 0;
};

class TokenManagerAuth final : public IGatewayAuth {
public:
    explicit TokenManagerAuth(TokenManager& tokens) : tokens_(tokens) {}
    Result<std::string> access_token() override { return tokens_.access_token(); }
    Result<std::string> refresh_after_unauthorized(const std::string& rejected) override {
        return tokens_.refresh_after_unauthorized(rejected);
    }
    void session_rejected(const std::string& reason) override { tokens_.session_rejected(reason); }

private:
    TokenManager& tokens_;
};

}  // namespace wb::model
