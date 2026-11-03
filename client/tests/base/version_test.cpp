#include "base/backoff.h"
#include "base/version.h"

#include <gtest/gtest.h>

using namespace wb;

TEST(Version, ParseAndCompare) {
    auto a = parse_version("v1.2.3");
    ASSERT_TRUE(a);
    EXPECT_EQ(a->major, 1);
    EXPECT_EQ(a->minor, 2);
    EXPECT_EQ(a->patch, 3);
    auto b = parse_version("1.2");
    ASSERT_TRUE(b);
    EXPECT_EQ(b->patch, 0);
    EXPECT_LT(compare_versions(*parse_version("1.2.3-beta.1"), *parse_version("1.2.3")), 0);
    EXPECT_LT(compare_versions(*parse_version("1.2.3-alpha"), *parse_version("1.2.3-beta")), 0);
    EXPECT_GT(compare_versions(*parse_version("1.10.0"), *parse_version("1.9.9")), 0);
    EXPECT_FALSE(parse_version("x.y"));
}

TEST(Version, BelowMinimum) {
    EXPECT_TRUE(below_minimum("0.1.0", "0.2.0"));
    EXPECT_FALSE(below_minimum("0.2.0", "0.2.0"));
    EXPECT_FALSE(below_minimum("0.3.0", ""));  // no minimum set
}

TEST(Backoff, GrowsWithJitterAndCaps) {
    Backoff b(std::chrono::milliseconds(1000), std::chrono::milliseconds(8000), 42);
    std::chrono::milliseconds prev_cap(0);
    for (int i = 0; i < 10; ++i) {
        const auto d = b.next();
        const auto cap = std::min(std::chrono::milliseconds(1000 << std::min(i, 3)), std::chrono::milliseconds(8000));
        EXPECT_GE(d, cap / 2);
        EXPECT_LE(d, cap);
        prev_cap = cap;
    }
    b.reset();
    EXPECT_LE(b.next(), std::chrono::milliseconds(1000));
}
