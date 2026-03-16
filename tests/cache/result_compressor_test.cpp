/**
 * @file result_compressor_test.cpp
 * @brief Unit tests for ResultCompressor Expected-based API
 */

#include "cache/result_compressor.h"

#include <gtest/gtest.h>

#include "utils/error.h"

namespace nvecd::cache {
namespace {

TEST(ResultCompressorTest, CompressAndDecompress) {
  std::vector<similarity::SimilarityResult> results = {
      {"item1", 0.95f},
      {"item2", 0.85f},
      {"item3", 0.75f},
  };

  auto compressed = ResultCompressor::CompressSimilarityResults(results);
  ASSERT_TRUE(compressed.has_value());
  EXPECT_FALSE(compressed->empty());

  // Calculate original size (must match SerializedSimilarityResult layout)
  const size_t original_size = results.size() * (256 + sizeof(float));

  auto decompressed = ResultCompressor::DecompressSimilarityResults(*compressed, original_size);
  ASSERT_TRUE(decompressed.has_value());
  ASSERT_EQ(decompressed->size(), results.size());

  for (size_t i = 0; i < results.size(); ++i) {
    EXPECT_EQ((*decompressed)[i].item_id, results[i].item_id);
    EXPECT_FLOAT_EQ((*decompressed)[i].score, results[i].score);
  }
}

TEST(ResultCompressorTest, CompressEmptyInput) {
  std::vector<similarity::SimilarityResult> empty_results;

  auto compressed = ResultCompressor::CompressSimilarityResults(empty_results);
  ASSERT_TRUE(compressed.has_value());
  EXPECT_TRUE(compressed->empty());
}

TEST(ResultCompressorTest, DecompressInvalidData) {
  // Create invalid compressed data
  std::vector<uint8_t> invalid_data = {0xFF, 0xFE, 0xFD, 0xFC, 0xFB};
  const size_t original_size = 256 + sizeof(float);  // 1 result

  auto result = ResultCompressor::DecompressSimilarityResults(invalid_data, original_size);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), utils::ErrorCode::kCacheDecompressionFailed);
}

TEST(ResultCompressorTest, DecompressTruncatedData) {
  // First compress valid data
  std::vector<similarity::SimilarityResult> results = {
      {"item1", 0.95f},
      {"item2", 0.85f},
  };

  auto compressed = ResultCompressor::CompressSimilarityResults(results);
  ASSERT_TRUE(compressed.has_value());

  // Truncate the compressed data
  std::vector<uint8_t> truncated(compressed->begin(), compressed->begin() + compressed->size() / 2);
  const size_t original_size = results.size() * (256 + sizeof(float));

  auto result = ResultCompressor::DecompressSimilarityResults(truncated, original_size);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), utils::ErrorCode::kCacheDecompressionFailed);
}

TEST(ResultCompressorTest, DecompressSizeMismatch) {
  // Compress 1 result but claim original_size for 2 results
  std::vector<similarity::SimilarityResult> results = {{"item1", 0.95f}};

  auto compressed = ResultCompressor::CompressSimilarityResults(results);
  ASSERT_TRUE(compressed.has_value());

  // Wrong original_size (double the actual)
  const size_t wrong_size = 2 * (256 + sizeof(float));

  auto result = ResultCompressor::DecompressSimilarityResults(*compressed, wrong_size);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), utils::ErrorCode::kCacheDecompressionFailed);
}

TEST(ResultCompressorTest, DecompressEmptyInput) {
  std::vector<uint8_t> empty_compressed;
  auto result = ResultCompressor::DecompressSimilarityResults(empty_compressed, 0);
  ASSERT_TRUE(result.has_value());
  EXPECT_TRUE(result->empty());
}

}  // namespace
}  // namespace nvecd::cache
