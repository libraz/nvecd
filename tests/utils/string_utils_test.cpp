/**
 * @file string_utils_test.cpp
 * @brief Unit tests for string utility functions
 */

#include "utils/string_utils.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace nvecd::utils;

TEST(ConstantTimeEqualsTest, EqualValuesMatch) {
  EXPECT_TRUE(ConstantTimeEquals("same-secret", "same-secret"));
}

TEST(ConstantTimeEqualsTest, DifferentValuesAndLengthsDoNotMatch) {
  EXPECT_FALSE(ConstantTimeEquals("same-secret", "same-secreT"));
  EXPECT_FALSE(ConstantTimeEquals("same-secret", "same-secret-extra"));
  EXPECT_FALSE(ConstantTimeEquals("", "x"));
}

// ========== FormatBytes tests ==========

TEST(FormatBytesTest, ZeroBytes) {
  EXPECT_EQ(FormatBytes(0), "0B");
}

TEST(FormatBytesTest, ByteRange) {
  // 500 >= 100, so precision is 0 decimal places
  EXPECT_EQ(FormatBytes(500), "500B");
}

TEST(FormatBytesTest, SmallByteRange) {
  // 5 < 10, so precision is 2 decimal places
  EXPECT_EQ(FormatBytes(5), "5.00B");
}

TEST(FormatBytesTest, KilobyteRange) {
  EXPECT_EQ(FormatBytes(1024), "1.00KB");
}

TEST(FormatBytesTest, MegabyteRange) {
  EXPECT_EQ(FormatBytes(1024 * 1024), "1.00MB");
}

TEST(FormatBytesTest, GigabyteRange) {
  size_t one_gb = 1024UL * 1024UL * 1024UL;
  EXPECT_EQ(FormatBytes(one_gb), "1.00GB");
}

TEST(FormatBytesTest, LargeValuePrecision) {
  // 150 MB = 150.0 MB (>= 100, so 0 decimal places)
  size_t bytes = 150UL * 1024UL * 1024UL;
  EXPECT_EQ(FormatBytes(bytes), "150MB");
}

TEST(FormatBytesTest, MediumValuePrecision) {
  // 15 MB (>= 10, so 1 decimal place)
  size_t bytes = 15UL * 1024UL * 1024UL;
  EXPECT_EQ(FormatBytes(bytes), "15.0MB");
}
