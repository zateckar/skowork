#include "base/encoding.h"
#include "base/jwt.h"

#include <gtest/gtest.h>

using namespace wb;

namespace {
std::string make(const json& h, const json& p, const std::string& sig = "sig") {
    return base64url_encode(h.dump()) + "." + base64url_encode(p.dump()) + "." + base64url_encode(sig);
}
}  // namespace

TEST(Jwt, ParsesClaims) {
    const std::string t = make({{"alg", "RS256"}, {"kid", "k1"}},
                               {{"iss", "https://idp/realms/r"}, {"sub", "u1"}, {"exp", 100}, {"iat", 50},
                                {"aud", json::array({"workbench-control-plane", "account"})},
                                {"groups", json::array({"g1", "g2"})}});
    auto j = parse_jwt(t);
    ASSERT_TRUE(j);
    EXPECT_EQ(j->alg(), "RS256");
    EXPECT_EQ(j->kid(), "k1");
    EXPECT_EQ(j->iss(), "https://idp/realms/r");
    EXPECT_EQ(j->sub(), "u1");
    EXPECT_EQ(j->exp(), 100);
    EXPECT_TRUE(j->has_audience("workbench-control-plane"));
    EXPECT_FALSE(j->has_audience("other"));
    EXPECT_EQ(j->string_list("groups"), (std::vector<std::string>{"g1", "g2"}));
    EXPECT_EQ(to_string(j->signature), "sig");
    EXPECT_EQ(j->signing_input, t.substr(0, t.rfind('.')));
}

TEST(Jwt, SingleStringAudience) {
    auto j = parse_jwt(make({{"alg", "ES256"}}, {{"aud", "workbench-desktop"}}));
    ASSERT_TRUE(j);
    EXPECT_TRUE(j->has_audience("workbench-desktop"));
}

TEST(Jwt, RejectsMalformed) {
    EXPECT_FALSE(parse_jwt("a.b"));
    EXPECT_FALSE(parse_jwt("!!.e30.e30"));
    EXPECT_FALSE(parse_jwt(base64url_encode(std::string_view("[]")) + "." + base64url_encode(std::string_view("{}")) + ".x"));
}
