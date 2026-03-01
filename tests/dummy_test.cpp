#include <gtest/gtest.h>

TEST(SanityCheck, TrueIsTrue) {
    EXPECT_TRUE(true);
}

TEST(SanityCheck, BasicArithmetic) {
    EXPECT_EQ(2 + 2, 4);
}

TEST(SanityCheck, StringNotEmpty) {
    std::string s = "embr";
    EXPECT_FALSE(s.empty());
    EXPECT_EQ(s.size(), 4);
}