/**
 * @file cache_key_test.cpp
 * @brief Unit tests for CacheKey and CacheKeyGenerator
 */

#include "cache/cache_key.h"

#include <gtest/gtest.h>

#include <unordered_map>

using namespace nvecd::cache;

TEST(CacheKeyTest, DefaultConstruction) {
  CacheKey key;
  EXPECT_EQ(key.hash_high, 0u);
  EXPECT_EQ(key.hash_low, 0u);
}

TEST(CacheKeyTest, ValueConstruction) {
  CacheKey key(0x1234, 0x5678);
  EXPECT_EQ(key.hash_high, 0x1234u);
  EXPECT_EQ(key.hash_low, 0x5678u);
}

TEST(CacheKeyTest, Equality) {
  CacheKey a(1, 2);
  CacheKey b(1, 2);
  EXPECT_EQ(a, b);
}

TEST(CacheKeyTest, InequalityDifferentHigh) {
  CacheKey a(1, 2);
  CacheKey b(3, 2);
  EXPECT_NE(a, b);
}

TEST(CacheKeyTest, InequalityDifferentLow) {
  CacheKey a(1, 2);
  CacheKey b(1, 3);
  EXPECT_NE(a, b);
}

TEST(CacheKeyTest, LessThanByHigh) {
  CacheKey a(1, 100);
  CacheKey b(2, 0);
  EXPECT_LT(a, b);
  EXPECT_FALSE(b < a);
}

TEST(CacheKeyTest, LessThanByLow) {
  CacheKey a(1, 2);
  CacheKey b(1, 3);
  EXPECT_LT(a, b);
  EXPECT_FALSE(b < a);
}

TEST(CacheKeyTest, LessThanEqual) {
  CacheKey a(1, 2);
  CacheKey b(1, 2);
  // Neither a < b nor b < a when equal
  EXPECT_FALSE(a < b);
  EXPECT_FALSE(b < a);
}

TEST(CacheKeyTest, CopyConstruction) {
  CacheKey original(0xABCD, 0xEF01);
  CacheKey copy(original);  // NOLINT(performance-unnecessary-copy-initialization)
  EXPECT_EQ(copy, original);
}

TEST(CacheKeyTest, CopyAssignment) {
  CacheKey original(0xABCD, 0xEF01);
  CacheKey copy;
  copy = original;
  EXPECT_EQ(copy, original);
}

TEST(CacheKeyTest, MoveConstruction) {
  CacheKey original(0xABCD, 0xEF01);
  CacheKey moved(std::move(original));
  EXPECT_EQ(moved.hash_high, 0xABCDu);
  EXPECT_EQ(moved.hash_low, 0xEF01u);
}

TEST(CacheKeyTest, ToString) {
  CacheKey key(0, 0);
  std::string str = key.ToString();
  // Should be a 32-character hex string
  EXPECT_EQ(str.size(), 32u);
}

TEST(CacheKeyTest, ToStringNonZero) {
  // Generate a key from known input to check ToString returns hex
  CacheKey key = CacheKeyGenerator::Generate("test");
  std::string str = key.ToString();
  EXPECT_EQ(str.size(), 32u);
  // All characters should be hex digits
  for (char c : str) {
    EXPECT_TRUE((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
  }
}

TEST(CacheKeyTest, HashFunction) {
  // Verify CacheKey works in unordered_map
  std::unordered_map<CacheKey, int> map;
  CacheKey key(123, 456);
  map[key] = 42;
  EXPECT_EQ(map[key], 42);
}

TEST(CacheKeyTest, GenerateDeterministic) {
  CacheKey a = CacheKeyGenerator::Generate("hello world");
  CacheKey b = CacheKeyGenerator::Generate("hello world");
  EXPECT_EQ(a, b);
}

TEST(CacheKeyTest, GenerateDifferentInputs) {
  CacheKey a = CacheKeyGenerator::Generate("hello");
  CacheKey b = CacheKeyGenerator::Generate("world");
  EXPECT_NE(a, b);
}
