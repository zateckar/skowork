// Server-sent events framing (WHATWG "Server-sent events", parsing section), used for streamed model responses
// (client/model-access Streaming Responses). Portable and allocation-bounded: the input is untrusted.
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace wb {

struct SseEvent {
    std::string event;  // "message" when the stream gave no event name
    std::string data;   // data lines joined with '\n'
    std::string id;
};

class SseParser {
public:
    explicit SseParser(size_t max_event_bytes = 8u * 1024 * 1024) : max_event_bytes_(max_event_bytes) {}

    // Feeds a chunk of the stream (any split, including in the middle of a line or of CR LF). Completed events are
    // appended to `out`. Returns false when one event grew beyond the size limit (the parser then ignores further
    // input).
    bool feed(std::string_view chunk, std::vector<SseEvent>& out);

    // True when bytes of an event that has not been completed by a blank line are pending. A stream that ends in
    // this state was cut (the incomplete event is never delivered).
    [[nodiscard]] bool has_partial() const { return !line_.empty() || has_data_ || !event_.empty() || !id_.empty(); }
    [[nodiscard]] bool failed() const { return failed_; }

private:
    void line_done(std::vector<SseEvent>& out);

    size_t max_event_bytes_;
    bool failed_ = false;
    bool at_start_ = true;
    bool last_cr_ = false;
    std::string line_;
    std::string data_;
    bool has_data_ = false;
    std::string event_;
    std::string id_;
};

}  // namespace wb
