#include "platform/dpapi.h"

#include <gtest/gtest.h>

using namespace wb;

TEST(Dpapi, RoundTripBoundToPurpose) {
    const bytes plain = {1, 2, 3, 4, 5};
    auto blob = dpapi_protect(plain, L"test/v1");
    ASSERT_TRUE(blob);
    EXPECT_NE(*blob, plain);
    auto back = dpapi_unprotect(*blob, L"test/v1");
    ASSERT_TRUE(back);
    EXPECT_EQ(*back, plain);
    EXPECT_FALSE(dpapi_unprotect(*blob, L"other/v1"));  // entropy includes the purpose
}

TEST(Dpapi, RejectsGarbage) {
    EXPECT_FALSE(dpapi_unprotect(bytes{0, 1, 2}, L"test/v1"));
}
