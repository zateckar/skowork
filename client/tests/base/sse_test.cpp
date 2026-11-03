// client/model-access Streaming Responses and Truncated Stream Detection: SSE framing on recorded streams.
#include "base/sse.h"

#include <gtest/gtest.h>

using namespace wb;

namespace {

std::vector<SseEvent> parse_all(const std::string& stream, size_t chunk) {
    SseParser p;
    std::vector<SseEvent> out;
    for (size_t i = 0; i < stream.size(); i += chunk) EXPECT_TRUE(p.feed(std::string_view(stream).substr(i, chunk), out));
    EXPECT_FALSE(p.has_partial());
    return out;
}

const std::string kStream =
    "data: {\"a\":1}\n\n"
    ": keep-alive\n\n"
    "event: ping\ndata: x\ndata: y\n\n"
    "data: [DONE]\n\n";

}  // namespace

TEST(Sse, ParsesEventsAtEverySplit) {
    for (const size_t chunk : {size_t{1}, size_t{2}, size_t{3}, size_t{7}, size_t{1000}}) {
        const auto events = parse_all(kStream, chunk);
        ASSERT_EQ(events.size(), 3U) << "chunk=" << chunk;
        EXPECT_EQ(events[0].event, "message");
        EXPECT_EQ(events[0].data, "{\"a\":1}");
        EXPECT_EQ(events[1].event, "ping");
        EXPECT_EQ(events[1].data, "x\ny");
        EXPECT_EQ(events[2].data, "[DONE]");
    }
}

TEST(Sse, AcceptsCrLfAndLoneCr) {
    std::string crlf = kStream, cr = kStream;
    for (size_t i = 0; i < crlf.size(); ++i)
        if (crlf[i] == '\n') {
            crlf.replace(i, 1, "\r\n");
            ++i;
        }
    for (auto& c : cr)
        if (c == '\n') c = '\r';
    for (const size_t chunk : {size_t{1}, size_t{5}, size_t{1000}}) {
        EXPECT_EQ(parse_all(crlf, chunk).size(), 3U);
        EXPECT_EQ(parse_all(cr, chunk).size(), 3U);
    }
}

TEST(Sse, SkipsBomAndIgnoresUnknownFields) {
    SseParser p;
    std::vector<SseEvent> out;
    ASSERT_TRUE(p.feed("\xEF\xBB\xBF" "retry: 100\nid: 7\ndata:no-space\n\n", out));
    ASSERT_EQ(out.size(), 1U);
    EXPECT_EQ(out[0].data, "no-space");
    EXPECT_EQ(out[0].id, "7");
}

TEST(Sse, CutStreamLeavesAPartialEventThatIsNeverDelivered) {
    SseParser p;
    std::vector<SseEvent> out;
    ASSERT_TRUE(p.feed("data: {\"complete\":true}\n\ndata: {\"cut", out));
    EXPECT_EQ(out.size(), 1U);
    EXPECT_TRUE(p.has_partial());

    SseParser q;
    out.clear();
    ASSERT_TRUE(q.feed("data: not terminated\n", out));  // line finished, event not
    EXPECT_TRUE(out.empty());
    EXPECT_TRUE(q.has_partial());
}

TEST(Sse, EventSizeIsBounded) {
    SseParser p(1024);
    std::vector<SseEvent> out;
    EXPECT_FALSE(p.feed(std::string(2000, 'x'), out));
    EXPECT_TRUE(p.failed());
    EXPECT_FALSE(p.feed("data: y\n\n", out));
}

TEST(Sse, EmptyDataEventIsDeliveredButEventWithoutDataIsNot) {
    SseParser p;
    std::vector<SseEvent> out;
    ASSERT_TRUE(p.feed("data:\n\nevent: only-name\n\n", out));
    ASSERT_EQ(out.size(), 1U);
    EXPECT_EQ(out[0].data, "");
}
