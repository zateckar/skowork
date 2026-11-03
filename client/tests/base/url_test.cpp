#include "base/url.h"

#include <gtest/gtest.h>

using namespace wb;

TEST(Url, ParsesHttpsWithDefaults) {
    auto u = parse_url("HTTPS://Example.COM/a/b?x=1#frag");
    ASSERT_TRUE(u);
    EXPECT_EQ(u->scheme, "https");
    EXPECT_EQ(u->host, "example.com");
    EXPECT_EQ(u->port, 443);
    EXPECT_EQ(u->path, "/a/b");
    EXPECT_EQ(u->query, "x=1");
    EXPECT_EQ(u->fragment, "frag");
    EXPECT_EQ(u->origin(), "https://example.com");
    EXPECT_EQ(u->path_and_query(), "/a/b?x=1");
}

TEST(Url, ExplicitPortAndLoopback) {
    auto u = parse_url("http://localhost:8180/realms/workbench");
    ASSERT_TRUE(u);
    EXPECT_EQ(u->port, 8180);
    EXPECT_TRUE(u->is_loopback());
    EXPECT_EQ(u->origin(), "http://localhost:8180");
    auto v6 = parse_url("http://[::1]:5000/");
    ASSERT_TRUE(v6);
    EXPECT_EQ(v6->host, "::1");
    EXPECT_TRUE(v6->is_loopback());
    auto ip = parse_url("http://127.0.0.1/");
    ASSERT_TRUE(ip);
    EXPECT_TRUE(ip->is_loopback());
    auto ext = parse_url("http://localhost.evil.com/");
    ASSERT_TRUE(ext);
    EXPECT_FALSE(ext->is_loopback());
}

TEST(Url, PrivateUseRedirectUri) {
    auto u = parse_url("com.example.workbench:/oauth2redirect?code=abc&state=xyz");
    ASSERT_TRUE(u);
    EXPECT_EQ(u->scheme, "com.example.workbench");
    EXPECT_EQ(u->path, "/oauth2redirect");
    const auto q = parse_query(u->query);
    EXPECT_EQ(q.find("code")->second, "abc");
    EXPECT_EQ(q.find("state")->second, "xyz");
}

TEST(Url, RejectsGarbage) {
    EXPECT_FALSE(parse_url(""));
    EXPECT_FALSE(parse_url("no scheme here"));
}

TEST(Url, EncodeDecode) {
    EXPECT_EQ(url_encode("a b&c=d/é"), "a%20b%26c%3Dd%2F%C3%A9");
    EXPECT_EQ(url_decode("a%20b+c%2"), "a b c%2");
    EXPECT_EQ(form_encode({{"grant_type", "authorization_code"}, {"redirect_uri", "com.example.workbench:/x"}}),
              "grant_type=authorization_code&redirect_uri=com.example.workbench%3A%2Fx");
}

TEST(Url, Join) {
    EXPECT_EQ(join_url("https://cp.example/", "/api/v1/x"), "https://cp.example/api/v1/x");
    EXPECT_EQ(join_url("https://cp.example", "api/v1/x"), "https://cp.example/api/v1/x");
}
