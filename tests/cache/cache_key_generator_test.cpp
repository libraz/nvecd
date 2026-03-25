/**
 * @file cache_key_generator_test.cpp
 * @brief Unit tests for SIM/SIMV cache key generation and HashVector
 */

#include "cache/cache_key_generator.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace nvecd::cache;

// ========== GenerateSimCacheKey tests ==========

TEST(GenerateSimCacheKeyTest, Deterministic) {
  CacheKey a = GenerateSimCacheKey("item1", 10, "vectors");
  CacheKey b = GenerateSimCacheKey("item1", 10, "vectors");
  EXPECT_EQ(a, b);
}

TEST(GenerateSimCacheKeyTest, DifferentId) {
  CacheKey a = GenerateSimCacheKey("item1", 10, "vectors");
  CacheKey b = GenerateSimCacheKey("item2", 10, "vectors");
  EXPECT_NE(a, b);
}

TEST(GenerateSimCacheKeyTest, DifferentTopK) {
  CacheKey a = GenerateSimCacheKey("item1", 10, "vectors");
  CacheKey b = GenerateSimCacheKey("item1", 20, "vectors");
  EXPECT_NE(a, b);
}

TEST(GenerateSimCacheKeyTest, DifferentMode) {
  CacheKey a = GenerateSimCacheKey("item1", 10, "vectors");
  CacheKey b = GenerateSimCacheKey("item1", 10, "events");
  EXPECT_NE(a, b);
}

TEST(GenerateSimCacheKeyTest, AllModes) {
  CacheKey vectors = GenerateSimCacheKey("id", 5, "vectors");
  CacheKey events = GenerateSimCacheKey("id", 5, "events");
  CacheKey fusion = GenerateSimCacheKey("id", 5, "fusion");
  EXPECT_NE(vectors, events);
  EXPECT_NE(vectors, fusion);
  EXPECT_NE(events, fusion);
}

// ========== GenerateSimvCacheKey tests ==========

TEST(GenerateSimvCacheKeyTest, Deterministic) {
  std::vector<float> vec = {1.0f, 2.0f, 3.0f};
  CacheKey a = GenerateSimvCacheKey(vec, 10, "vectors");
  CacheKey b = GenerateSimvCacheKey(vec, 10, "vectors");
  EXPECT_EQ(a, b);
}

TEST(GenerateSimvCacheKeyTest, DifferentVector) {
  std::vector<float> vec1 = {1.0f, 2.0f, 3.0f};
  std::vector<float> vec2 = {4.0f, 5.0f, 6.0f};
  CacheKey a = GenerateSimvCacheKey(vec1, 10, "vectors");
  CacheKey b = GenerateSimvCacheKey(vec2, 10, "vectors");
  EXPECT_NE(a, b);
}

TEST(GenerateSimvCacheKeyTest, DifferentTopK) {
  std::vector<float> vec = {1.0f, 2.0f, 3.0f};
  CacheKey a = GenerateSimvCacheKey(vec, 10, "vectors");
  CacheKey b = GenerateSimvCacheKey(vec, 20, "vectors");
  EXPECT_NE(a, b);
}

TEST(GenerateSimvCacheKeyTest, EmptyVector) {
  std::vector<float> empty_vec;
  CacheKey a = GenerateSimvCacheKey(empty_vec, 10, "vectors");
  CacheKey b = GenerateSimvCacheKey(empty_vec, 10, "vectors");
  EXPECT_EQ(a, b);
}

// ========== HashVector tests ==========

TEST(HashVectorTest, EmptyVector) {
  std::vector<float> empty_vec;
  std::string hash = HashVector(empty_vec);
  // Empty vector should return zero hash (32 zeros)
  EXPECT_EQ(hash, std::string(32, '0'));
}

TEST(HashVectorTest, NormalVector) {
  std::vector<float> vec = {1.0f, 2.0f, 3.0f};
  std::string hash = HashVector(vec);
  // Should be 32-character hex string
  EXPECT_EQ(hash.size(), 32u);
  for (char c : hash) {
    EXPECT_TRUE((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
  }
}

TEST(HashVectorTest, Deterministic) {
  std::vector<float> vec = {0.5f, -1.5f, 3.14f};
  std::string hash1 = HashVector(vec);
  std::string hash2 = HashVector(vec);
  EXPECT_EQ(hash1, hash2);
}

TEST(HashVectorTest, DifferentVectors) {
  std::vector<float> vec1 = {1.0f, 2.0f, 3.0f};
  std::vector<float> vec2 = {1.0f, 2.0f, 3.1f};
  EXPECT_NE(HashVector(vec1), HashVector(vec2));
}

TEST(HashVectorTest, SingleElement) {
  std::vector<float> vec = {42.0f};
  std::string hash = HashVector(vec);
  EXPECT_EQ(hash.size(), 32u);
  // Should not be all zeros
  EXPECT_NE(hash, std::string(32, '0'));
}

TEST(HashVectorTest, OrderMatters) {
  std::vector<float> vec1 = {1.0f, 2.0f};
  std::vector<float> vec2 = {2.0f, 1.0f};
  EXPECT_NE(HashVector(vec1), HashVector(vec2));
}
