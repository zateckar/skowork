#include "base/encoding.h"
#include "base/redact.h"

#include <gtest/gtest.h>

using namespace wb;

TEST(Redact, KeyedSecrets) {
    const std::string out = redact_secrets("POST token: refresh_token=abc123def&client_id=skowork-desktop");
    EXPECT_EQ(out.find("abc123def"), std::string::npos);
    EXPECT_NE(out.find("client_id=skowork-desktop"), std::string::npos);
    const std::string hdr = redact_secrets("Authorization: Bearer eyJhbGciOi.payload.sig");
    EXPECT_EQ(hdr.find("payload.sig"), std::string::npos);
}

TEST(Redact, JwtShapedAnywhere) {
    const std::string jwt = base64url_encode(std::string_view(R"({"alg":"RS256","kid":"k"})")) + "." +
                            base64url_encode(std::string_view(R"({"sub":"user","exp":1})")) + ".c2lnbmF0dXJl";
    const std::string out = redact_secrets("got " + jwt + " from idp");
    EXPECT_EQ(out.find(jwt), std::string::npos);
    EXPECT_NE(out.find("from idp"), std::string::npos);
    EXPECT_TRUE(looks_secret_bearing(jwt));
}

TEST(Redact, LongOpaqueTokens) {
    const std::string tok(48, 'A');
    EXPECT_EQ(redact_secrets("code " + tok).find(tok), std::string::npos);
    EXPECT_EQ(redact_secrets("installation 3f2c1a7e-1111-4222-8333-444455556666"),
              "installation 3f2c1a7e-1111-4222-8333-444455556666");
}

TEST(Redact, PlainTextUntouched) {
    EXPECT_EQ(redact_secrets("control plane connected"), "control plane connected");
    EXPECT_FALSE(looks_secret_bearing("hello world"));
}
