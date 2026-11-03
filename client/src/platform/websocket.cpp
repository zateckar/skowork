#include "platform/websocket.h"

#include "base/url.h"
#include "platform/http.h"
#include "platform/win.h"

#include <winhttp.h>

namespace wb {
namespace {

struct hinternet_traits {
    using type = HINTERNET;
    static HINTERNET invalid() noexcept { return nullptr; }
    static void close(HINTERNET h) noexcept { ::WinHttpCloseHandle(h); }
};
using unique_hinternet = unique_res<hinternet_traits>;

}  // namespace

WebSocket::~WebSocket() { abort(); }

Result<std::unique_ptr<WebSocket>> WebSocket::connect(const std::string& url,
                                                      const std::vector<std::pair<std::string, std::string>>& headers,
                                                      std::chrono::milliseconds timeout) {
    if (!HttpClient::url_allowed(url)) return make_error("insecure_url", "only wss endpoints are allowed");
    const auto u = parse_url(url);
    if (!u) return make_error("url", "invalid URL");
    const bool secure = u->scheme == "wss";
    HINTERNET session = static_cast<HINTERNET>(http().session());
    unique_hinternet connect(::WinHttpConnect(session, widen(u->host).c_str(), static_cast<INTERNET_PORT>(u->port), 0));
    if (!connect) return make_error("connect", error_text(::GetLastError()));
    unique_hinternet req(::WinHttpOpenRequest(connect.get(), L"GET", widen(u->path_and_query()).c_str(), nullptr,
                                              WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                              secure ? WINHTTP_FLAG_SECURE : 0));
    if (!req) return make_error("connect", error_text(::GetLastError()));
    const int ms = static_cast<int>(timeout.count());
    ::WinHttpSetTimeouts(req.get(), ms, ms, ms, ms);
    DWORD logon = WINHTTP_AUTOLOGON_SECURITY_LEVEL_HIGH;
    ::WinHttpSetOption(req.get(), WINHTTP_OPTION_AUTOLOGON_POLICY, &logon, sizeof logon);
    if (u->is_loopback()) {
        WINHTTP_PROXY_INFO direct{WINHTTP_ACCESS_TYPE_NO_PROXY, nullptr, nullptr};
        ::WinHttpSetOption(req.get(), WINHTTP_OPTION_PROXY, &direct, sizeof direct);
    }
    if (!::WinHttpSetOption(req.get(), WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0))
        return make_error("io", "upgrade option: " + error_text(::GetLastError()));
    std::wstring hdrs;
    for (const auto& [k, v] : headers) hdrs += widen(k) + L": " + widen(v) + L"\r\n";

    bool sent_without_cert = false;
    for (int attempt = 0; attempt < 3; ++attempt) {
        if (!::WinHttpSendRequest(req.get(), hdrs.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : hdrs.c_str(),
                                  hdrs.empty() ? 0 : static_cast<DWORD>(-1L), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
            !::WinHttpReceiveResponse(req.get(), nullptr)) {
            const DWORD e = ::GetLastError();
            // Optional TLS client certificate requested (see platform/http.cpp): continue without one, once.
            if (e == ERROR_WINHTTP_CLIENT_AUTH_CERT_NEEDED && !sent_without_cert) {
                sent_without_cert = true;
                ::WinHttpSetOption(req.get(), WINHTTP_OPTION_CLIENT_CERT_CONTEXT, WINHTTP_NO_CLIENT_CERT_CONTEXT, 0);
                continue;
            }
            return make_error(e == ERROR_WINHTTP_TIMEOUT ? "timeout" : "connect", error_text(e));
        }
        DWORD status = 0, sz = sizeof status;
        ::WinHttpQueryHeaders(req.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                              WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX);
        if (status == 101) break;
        if (status == HTTP_STATUS_PROXY_AUTH_REQ && attempt == 0) {
            DWORD supported = 0, first = 0, target = 0;
            if (::WinHttpQueryAuthSchemes(req.get(), &supported, &first, &target)) {
                const DWORD scheme = (supported & WINHTTP_AUTH_SCHEME_NEGOTIATE) ? WINHTTP_AUTH_SCHEME_NEGOTIATE
                                     : (supported & WINHTTP_AUTH_SCHEME_NTLM) ? WINHTTP_AUTH_SCHEME_NTLM
                                                                              : 0;
                if (scheme) {
                    ::WinHttpSetCredentials(req.get(), WINHTTP_AUTH_TARGET_PROXY, scheme, nullptr, nullptr, nullptr);
                    continue;
                }
            }
        }
        return make_error("http_" + std::to_string(status), "WebSocket handshake refused");
    }
    HINTERNET ws = ::WinHttpWebSocketCompleteUpgrade(req.get(), 0);
    if (!ws) return make_error("io", "complete upgrade: " + error_text(::GetLastError()));
    // Idle connections are kept alive by the server's pings and the client's heartbeats; receive blocks indefinitely.
    DWORD infinite = 0;
    ::WinHttpSetOption(ws, WINHTTP_OPTION_RECEIVE_TIMEOUT, &infinite, sizeof infinite);
    std::unique_ptr<WebSocket> sock(new WebSocket());
    sock->ws_ = ws;
    return sock;
}

Status WebSocket::send_text(const std::string& text) {
    const std::lock_guard lock(mx_);
    if (!ws_ || closed_) return make_error("closed", "socket closed");
    const DWORD rc = ::WinHttpWebSocketSend(static_cast<HINTERNET>(ws_), WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
                                            const_cast<char*>(text.data()), static_cast<DWORD>(text.size()));
    if (rc != NO_ERROR) return make_error("io", error_text(rc));
    return {};
}

Result<WsMessage> WebSocket::receive(size_t max_message) {
    HINTERNET ws = nullptr;
    {
        const std::lock_guard lock(mx_);
        if (!ws_ || closed_) return make_error("closed", "socket closed");
        ws = static_cast<HINTERNET>(ws_);
    }
    std::string msg;
    std::string buf(64 * 1024, '\0');
    for (;;) {
        DWORD got = 0;
        WINHTTP_WEB_SOCKET_BUFFER_TYPE type{};
        const DWORD rc = ::WinHttpWebSocketReceive(ws, buf.data(), static_cast<DWORD>(buf.size()), &got, &type);
        if (rc != NO_ERROR) return make_error("io", error_text(rc));
        if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) {
            WsMessage m;
            m.kind = WsMessage::Kind::closed;
            USHORT status = 0;
            DWORD reason_len = 0;
            char reason[WINHTTP_WEB_SOCKET_MAX_CLOSE_REASON_LENGTH];
            if (::WinHttpWebSocketQueryCloseStatus(ws, &status, reason, sizeof reason, &reason_len) == NO_ERROR) {
                m.close_status = status;
                m.close_reason.assign(reason, reason_len);
            }
            return m;
        }
        if (type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE || type == WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE)
            return make_error("protocol", "binary frames are not used");
        msg.append(buf.data(), got);
        if (msg.size() > max_message) return make_error("too_large", "message too large");
        if (type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE) return WsMessage{WsMessage::Kind::text, std::move(msg), 0, {}};
    }
}

void WebSocket::close(unsigned short status, const std::string& reason) {
    const std::lock_guard lock(mx_);
    if (!ws_ || closed_) return;
    closed_ = true;
    ::WinHttpWebSocketShutdown(static_cast<HINTERNET>(ws_), status,
                               reason.empty() ? nullptr : const_cast<char*>(reason.data()),
                               static_cast<DWORD>(reason.size()));
    ::WinHttpCloseHandle(static_cast<HINTERNET>(ws_));
    ws_ = nullptr;
}

void WebSocket::abort() {
    const std::lock_guard lock(mx_);
    if (!ws_) return;
    closed_ = true;
    ::WinHttpCloseHandle(static_cast<HINTERNET>(ws_));
    ws_ = nullptr;
}

}  // namespace wb
