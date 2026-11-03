#include "base/strings.h"

#include <gtest/gtest.h>

using namespace wb;

TEST(Strings, Basics) {
    EXPECT_EQ(split("a,b,,c", ','), (std::vector<std::string>{"a", "b", "", "c"}));
    EXPECT_EQ(trim("  x y \t\n"), "x y");
    EXPECT_EQ(to_lower("Workbench"), "workbench");
    EXPECT_TRUE(starts_with("com.example.workbench:/x", "com.example.workbench:"));
    EXPECT_TRUE(ends_with("file.db", ".db"));
    EXPECT_TRUE(iequals("Bearer", "bearer"));
    EXPECT_TRUE(constant_time_equals("abc", "abc"));
    EXPECT_FALSE(constant_time_equals("abc", "abd"));
    EXPECT_FALSE(constant_time_equals("abc", "ab"));
}

TEST(Strings, Utf8TruncateNeverSplitsACodePoint) {
    const std::string s = "aé€😀";  // 1 + 2 + 3 + 4 bytes
    EXPECT_EQ(utf8_truncate(s, 2), "a");
    EXPECT_EQ(utf8_truncate(s, 3), "aé");
    EXPECT_EQ(utf8_truncate(s, 8), "aé€");
    EXPECT_EQ(utf8_truncate(s, 100), s);
}
