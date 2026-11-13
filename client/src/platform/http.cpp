#include "platform/http.h"

#include "base/strings.h"
#include "base/url.h"
#include "platform/win.h"

#include <winhttp.h>

#include <algorithm>

namespace wb {
namespace {

struct hinternet_traits {
    using type = HINTERNET;
    static HINTERNET invalid() noexcept { return nullptr; }
    static void close(HINTERNET h) noexcept { ::WinHttpCloseHandle(h); }
};
using unique_hinternet = unique_res<hinternet_traits>;

Error map_error(DWORD code, const CancelToken* cancel) {
    if (cancel && cancel->cancelled()) return make_error("cancelled", "request cancelled");
    switch (code) {
    case ERROR_WINHTTP_TIMEOUT: return make_error("timeout", error_text(code));
    case ERROR_WINHTTP_SECURE_FAILURE:
    case ERROR_WINHTTP_SECURE_CERT_CN_INVALID:
    case ERROR_WINHTTP_SECURE_CERT_DATE_INVALID:
    case ERROR_WINHTTP_SECURE_INVALID_CA:
    case ERROR_WINHTTP_SECURE_CERT_REV_FAILED:
    case ERROR_WINHTTP_SECURE_INVALID_CERT:
    case ERROR_WINHTTP_SECURE_CERT_REVOKED:
    case ERROR_WINHTTP_SECURE_CERT_WRONG_USAGE:
    case ERROR_WINHTTP_SECURE_CHANNEL_ERROR: return make_error("tls", error_text(code));
    case ERROR_WINHTTP_CANNOT_CONNECT:
    case ERROR_WINHTTP_NAME_NOT_RESOLVED:
    case ERROR_WINHTTP_CONNECTION_ERROR: return make_error("connect", error_text(code));
    case ERROR_WINHTTP_OPERATION_CANCELLED:
    case ERROR_INVALID_HANDLE: return make_error("cancelled", error_text(code));
    default: return make_error("io", error_text(code));
    }
}

std::map<std::string, std::string> read_headers(HINTERNET req) {
    std::map<std::string, std::string> out;
    DWORD size = 0;
    ::WinHttpQueryHeaders(req, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &size,
                          WINHTTP_NO_HEADER_INDEX);
    if (size == 0) return out;
    std::wstring raw(size / sizeof(wchar_t), L'\0');
    if (!::WinHttpQueryHeaders(req, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, raw.data(), &size,
                               WINHTTP_NO_HEADER_INDEX))
        return out;
    for (const auto& line : split(narrow(raw), '\n')) {
        const auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        out[to_lower(trim(line.substr(0, colon)))] = trim(line.substr(colon + 1));
    }
    return out;
}

}  // namespace

void CancelToken::cancel() {
    cancelled_ = true;
    const std::lock_guard lock(mx_);
    // Closing the handle from another thread aborts a blocking WinHTTP call on it.
    if (handle_) ::WinHttpCloseHandle(static_cast<HINTERNET>(handle_));
    handle_ = nullptr;
}

bool CancelToken::attach(void* handle) {
    const std::lock_guard lock(mx_);
    if (cancelled_) return false;
    handle_ = handle;
    return true;
}

void CancelToken::detach() {
    const std::lock_guard lock(mx_);
    handle_ = nullptr;
}

namespace {
#ifdef SKOWORK_DEV_ENDPOINTS
// Debug and test builds: SKOWORK_DEV_PROXY=http://127.0.0.1:3128 sends non-loopback requests through
// dev/scripts/dev-proxy.mjs, for machines that block outbound connections from unsigned local builds.
std::wstring dev_proxy() {
    wchar_t buf[256];
    const DWORD n = ::GetEnvironmentVariableW(L"SKOWORK_DEV_PROXY", buf, 256);
    std::wstring v = n > 0 && n < 256 ? std::wstring(buf, n) : std::wstring{};
    if (v.rfind(L"http://", 0) == 0) v = v.substr(7);
    while (!v.empty() && v.back() == L'/') v.pop_back();
    return v;
}
#endif
}  // namespace

HttpClient::HttpClient(std::wstring user_agent) {
#ifdef SKOWORK_DEV_ENDPOINTS
    if (const std::wstring proxy = dev_proxy(); !proxy.empty())
        session_ = ::WinHttpOpen(user_agent.c_str(), WINHTTP_ACCESS_TYPE_NAMED_PROXY, proxy.c_str(), L"<local>", 0);
    else
#endif
        session_ = ::WinHttpOpen(user_agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                 WINHTTP_NO_PROXY_BYPASS, 0);
    if (session_) {
        // TLS 1.2+ only; HTTP/2 where the server offers it.
        DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
        ::WinHttpSetOption(session_, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof protocols);
        DWORD redirect = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
        ::WinHttpSetOption(session_, WINHTTP_OPTION_REDIRECT_POLICY, &redirect, sizeof redirect);
    }
}

HttpClient::~HttpClient() {
    if (session_) ::WinHttpCloseHandle(session_);
}

bool HttpClient::url_allowed(std::string_view url) {
    const auto u = parse_url(url);
    if (!u) return false;
    if (u->scheme == "https" || u->scheme == "wss") return true;
#ifdef SKOWORK_DEV_ENDPOINTS
    if ((u->scheme == "http" || u->scheme == "ws") && u->is_loopback()) return true;
#endif
    return false;
}

Result<HttpResponse> HttpClient::send(const HttpRequest& req) {
    if (!session_) return make_error("io", "WinHTTP session unavailable");
    const auto u = parse_url(req.url);
    if (!u || u->host.empty()) return make_error("url", "invalid URL");
    if (!url_allowed(req.url) && !(req.allow_plain_http && u->scheme == "http"))
        return make_error("insecure_url", "only https endpoints are allowed");
    const bool secure = u->scheme == "https";

    unique_hinternet connect(::WinHttpConnect(session_, widen(u->host).c_str(), static_cast<INTERNET_PORT>(u->port), 0));
    if (!connect) return map_error(::GetLastError(), req.cancel.get());
    unique_hinternet r(::WinHttpOpenRequest(connect.get(), widen(req.method).c_str(), widen(u->path_and_query()).c_str(),
                                            nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                            secure ? WINHTTP_FLAG_SECURE : 0));
    if (!r) return map_error(::GetLastError(), req.cancel.get());

    const int connect_ms = static_cast<int>(req.connect_timeout.count());
    const int idle_ms = static_cast<int>(req.idle_timeout.count());
    ::WinHttpSetTimeouts(r.get(), connect_ms, connect_ms, idle_ms, idle_ms);
    // Never send Windows credentials to servers: the bearer token is the only authentication. A corporate proxy's 407 is
    // answered below.
    DWORD logon = WINHTTP_AUTOLOGON_SECURITY_LEVEL_HIGH;
    ::WinHttpSetOption(r.get(), WINHTTP_OPTION_AUTOLOGON_POLICY, &logon, sizeof logon);
    // For loopback hosts the autologon policy does not stop WinHTTP from starting a Negotiate/NTLM handshake with a
    // server that asks for one (observed with SECURITY_LEVEL_HIGH), so automatic authentication is switched off there;
    // loopback never goes through a proxy, so the 407 handling below is unaffected.
    const bool no_auto_auth = u->is_loopback();
    DWORD disabled = (req.follow_redirects ? 0 : WINHTTP_DISABLE_REDIRECTS) | (no_auto_auth ? WINHTTP_DISABLE_AUTHENTICATION : 0);
    if (disabled) ::WinHttpSetOption(r.get(), WINHTTP_OPTION_DISABLE_FEATURE, &disabled, sizeof disabled);
    if (u->is_loopback()) {
        WINHTTP_PROXY_INFO direct{WINHTTP_ACCESS_TYPE_NO_PROXY, nullptr, nullptr};
        ::WinHttpSetOption(r.get(), WINHTTP_OPTION_PROXY, &direct, sizeof direct);
    }

    std::wstring headers;
    for (const auto& [k, v] : req.headers) headers += widen(k) + L": " + widen(v) + L"\r\n";

    if (req.cancel && !req.cancel->attach(r.get())) return make_error("cancelled", "request cancelled");
    struct detach_guard {
        CancelToken* t;
        ~detach_guard() {
            if (t) t->detach();
        }
    } guard{req.cancel.get()};

    bool sent_without_cert = false;
    for (int attempt = 0;; ++attempt) {
        if (!::WinHttpSendRequest(r.get(), headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                                  headers.empty() ? 0 : static_cast<DWORD>(-1L),
                                  req.body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(req.body.data()),
                                  static_cast<DWORD>(req.body.size()), static_cast<DWORD>(req.body.size()), 0) ||
            !::WinHttpReceiveResponse(r.get(), nullptr)) {
            const DWORD err = ::GetLastError();
            // A server that asks for an optional TLS client certificate (common on organization gateways): continue
            // without one, once. The bearer token is the only authentication.
            if (err == ERROR_WINHTTP_CLIENT_AUTH_CERT_NEEDED && !sent_without_cert) {
                sent_without_cert = true;
                ::WinHttpSetOption(r.get(), WINHTTP_OPTION_CLIENT_CERT_CONTEXT, WINHTTP_NO_CLIENT_CERT_CONTEXT, 0);
                continue;
            }
            return map_error(err, req.cancel.get());
        }
        DWORD status = 0, sz = sizeof status;
        ::WinHttpQueryHeaders(r.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                              WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX);
        if (status == HTTP_STATUS_PROXY_AUTH_REQ && attempt == 0) {
            // Corporate proxy: answer with the logged-on user's credentials, proxy target only.
            DWORD supported = 0, first = 0, target = 0;
            if (::WinHttpQueryAuthSchemes(r.get(), &supported, &first, &target)) {
                const DWORD scheme = (supported & WINHTTP_AUTH_SCHEME_NEGOTIATE) ? WINHTTP_AUTH_SCHEME_NEGOTIATE
                                     : (supported & WINHTTP_AUTH_SCHEME_NTLM) ? WINHTTP_AUTH_SCHEME_NTLM
                                                                              : 0;
                if (scheme) {
                    ::WinHttpSetCredentials(r.get(), WINHTTP_AUTH_TARGET_PROXY, scheme, nullptr, nullptr, nullptr);
                    continue;
                }
            }
        }
        HttpResponse resp;
        resp.status = static_cast<int>(status);
        resp.headers = read_headers(r.get());
        const bool stream_body = req.on_chunk && (!req.stream_success_only || (status >= 200 && status < 300));
        std::string buf(16 * 1024, '\0');
        for (;;) {
            // A synchronous WinHttpReadData waits until its buffer is full or the body ends, which holds back SSE
            // keep-alives and small deltas (fix-long-thread-model-calls D8). Read only what is available.
            DWORD avail = 0;
            if (!::WinHttpQueryDataAvailable(r.get(), &avail)) return map_error(::GetLastError(), req.cancel.get());
            if (avail == 0) break;
            DWORD got = 0;
            const DWORD want = std::min<DWORD>(avail, static_cast<DWORD>(buf.size()));
            if (!::WinHttpReadData(r.get(), buf.data(), want, &got))
                return map_error(::GetLastError(), req.cancel.get());
            if (got == 0) break;
            const std::string_view chunk(buf.data(), got);
            if (stream_body) {
                if (!req.on_chunk(chunk)) return make_error("aborted", "stream aborted by caller");
            } else {
                if (resp.body.size() + got > req.max_body) return make_error("too_large", "response body too large");
                resp.body.append(chunk);
            }
        }
        return resp;
    }
}

HttpClient& http() {
    static HttpClient client(L"Skowork");
    return client;
}

}  // namespace wb
