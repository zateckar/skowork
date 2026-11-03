// The Chrome DevTools Protocol over --remote-debugging-pipe (client/browser-use Pipe-Only Control Channel;
// add-browser-use design D1): NUL-terminated UTF-8 JSON messages in both directions. Portable, so the fuzz job builds it.
#pragma once

#include "base/json.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace wb::browser {

// Splits the byte stream from Chrome into messages.
class CdpFramer {
public:
    explicit CdpFramer(std::size_t max_message = std::size_t{64} << 20) : max_(max_message) {}

    // Appends `bytes` and moves every complete message to `out`. Returns false once a message grows beyond the limit;
    // the stream is unusable after that.
    bool feed(std::string_view bytes, std::vector<std::string>& out);
    [[nodiscard]] std::size_t buffered() const { return buf_.size(); }

private:
    std::size_t max_;
    std::string buf_;
    bool broken_ = false;
};

struct CdpMessage {
    enum class Kind { response, event, invalid };
    Kind kind = Kind::invalid;
    long long id = 0;                   // response
    json result = nullptr;              // response
    std::string error;                  // response: the error message, empty on success
    std::string method;                 // event
    json params = json::object();       // event
    std::string session_id;             // both: the target session, empty for the browser
};

// Parses one message; anything that is not a response or an event is Kind::invalid.
CdpMessage parse_cdp_message(std::string_view text);

// A command as written to the pipe, with its NUL terminator.
std::string encode_cdp_command(long long id, std::string_view method, const json& params, std::string_view session_id = {});

}  // namespace wb::browser
