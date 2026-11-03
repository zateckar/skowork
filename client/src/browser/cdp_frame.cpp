#include "browser/cdp_frame.h"

namespace wb::browser {

bool CdpFramer::feed(std::string_view bytes, std::vector<std::string>& out) {
    if (broken_) return false;
    std::size_t start = 0;
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (bytes[i] != '\0') continue;
        buf_.append(bytes.substr(start, i - start));
        if (buf_.size() > max_) {
            broken_ = true;
            buf_.clear();
            return false;
        }
        out.push_back(std::move(buf_));
        buf_.clear();
        start = i + 1;
    }
    buf_.append(bytes.substr(start));
    if (buf_.size() > max_) {
        broken_ = true;
        buf_.clear();
        return false;
    }
    return true;
}

CdpMessage parse_cdp_message(std::string_view text) {
    CdpMessage m;
    auto j = parse_json(text);
    if (!j || !j->is_object()) return m;
    m.session_id = jstr(*j, "sessionId");
    if (j->contains("id")) {
        const json& id = (*j)["id"];
        if (!id.is_number_integer()) return m;
        m.kind = CdpMessage::Kind::response;
        m.id = id.get<long long>();
        if (j->contains("error")) {
            const json& e = (*j)["error"];
            m.error = e.is_object() ? jstr(e, "message", "error") : std::string("error");
            if (m.error.empty()) m.error = "error";
        } else {
            m.result = j->contains("result") ? (*j)["result"] : json::object();
        }
        return m;
    }
    if (j->contains("method") && (*j)["method"].is_string() && !(*j)["method"].get_ref<const std::string&>().empty()) {
        m.kind = CdpMessage::Kind::event;
        m.method = (*j)["method"].get<std::string>();
        if (j->contains("params") && (*j)["params"].is_object()) m.params = (*j)["params"];
    }
    return m;
}

std::string encode_cdp_command(long long id, std::string_view method, const json& params, std::string_view session_id) {
    json j = {{"id", id}, {"method", std::string(method)}, {"params", params.is_object() ? params : json::object()}};
    if (!session_id.empty()) j["sessionId"] = std::string(session_id);
    std::string s = j.dump(-1, ' ', false, json::error_handler_t::replace);
    s.push_back('\0');
    return s;
}

}  // namespace wb::browser
