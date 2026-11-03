#include "base/encoding.h"

#include <gtest/gtest.h>

#include <cstring>

using namespace wb;

TEST(Encoding, Base64RoundTripAndRfc4648Vectors) {
    const std::pair<const char*, const char*> v[] = {{"", ""}, {"f", "Zg=="}, {"fo", "Zm8="}, {"foo", "Zm9v"},
                                                      {"foob", "Zm9vYg=="}, {"fooba", "Zm9vYmE="}, {"foobar", "Zm9vYmFy"}};
    for (const auto& [plain, enc] : v) {
        const bytes b(plain, plain + std::strlen(plain));
        EXPECT_EQ(base64_encode(b), enc);
        auto d = base64_decode(enc);
        ASSERT_TRUE(d);
        EXPECT_EQ(to_string(*d), plain);
    }
}

TEST(Encoding, Base64UrlHasNoPaddingAndUrlAlphabet) {
    const bytes b = {0xfb, 0xff, 0xfe};
    EXPECT_EQ(base64url_encode(b), "-__-");
    EXPECT_EQ(base64url_encode(std::string_view("f")), "Zg");
    auto d = base64url_decode("-__-");
    ASSERT_TRUE(d);
    EXPECT_EQ(*d, b);
    EXPECT_TRUE(base64url_decode("Zg"));
}

TEST(Encoding, RejectsInvalidInput) {
    EXPECT_FALSE(base64_decode("Zm9v!"));
    EXPECT_FALSE(base64url_decode("Zm+v"));
    EXPECT_FALSE(base64url_decode("Z"));
}

TEST(Encoding, Hex) {
    const bytes b = {0x00, 0xab, 0xff};
    EXPECT_EQ(hex_encode(b), "00abff");
}
