#include "base/sse.h"

namespace wb {

bool SseParser::feed(std::string_view chunk, std::vector<SseEvent>& out) {
    if (failed_) return false;
    if (at_start_ && !chunk.empty()) {
        at_start_ = false;
        if (chunk.substr(0, 3) == "\xEF\xBB\xBF") chunk.remove_prefix(3);  // UTF-8 byte order mark
    }
    for (const char c : chunk) {
        if (last_cr_) {
            last_cr_ = false;
            if (c == '\n') continue;  // the LF of a CR LF pair
        }
        if (c == '\r' || c == '\n') {
            last_cr_ = c == '\r';
            line_done(out);
            if (failed_) return false;
            continue;
        }
        line_.push_back(c);
        if (line_.size() + data_.size() > max_event_bytes_) {
            failed_ = true;
            return false;
        }
    }
    return true;
}

void SseParser::line_done(std::vector<SseEvent>& out) {
    std::string line = std::move(line_);
    line_.clear();
    if (line.empty()) {
        // Blank line: dispatch the event when it has data.
        if (has_data_) {
            SseEvent e;
            e.event = event_.empty() ? "message" : std::move(event_);
            e.data = std::move(data_);
            e.id = std::move(id_);
            out.push_back(std::move(e));
        }
        data_.clear();
        has_data_ = false;
        event_.clear();
        id_.clear();
        return;
    }
    if (line.front() == ':') return;  // comment (keep-alive)
    std::string_view field = line;
    std::string_view value;
    if (const auto colon = line.find(':'); colon != std::string::npos) {
        field = std::string_view(line).substr(0, colon);
        value = std::string_view(line).substr(colon + 1);
        if (!value.empty() && value.front() == ' ') value.remove_prefix(1);
    }
    if (field == "data") {
        if (has_data_) data_.push_back('\n');
        data_.append(value);
        has_data_ = true;
    } else if (field == "event") {
        event_.assign(value);
    } else if (field == "id") {
        id_.assign(value);
    }
    // "retry" and unknown fields are ignored.
}

}  // namespace wb
