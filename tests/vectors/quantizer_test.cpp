/**
 * @file quantizer_test.cpp
 * @brief Tests for ScalarQuantizer
 */

#include "vectors/quantizer.h"

#include <gtest/gtest.h>

#include <cmath>
#include <numeric>
#include <random>
#include <vector>

namespace nvecd::vectors {
namespace {

// ============================================================================
// Helpers
// ============================================================================

std::vector<float> MakeRandomVectors(uint32_t count, uint32_t dim, std::mt19937& rng, float min_val = -1.0F,
                                     float max_val = 1.0F) {
  std::uniform_real_distribution<float> dist(min_val, max_val);
  std::vector<float> data(static_cast<size_t>(count) * dim);
  for (auto& v : data) {
    v = dist(rng);
  }
  return data;
}

float FloatDotProduct(const float* a, const float* b, uint32_t dim) {
  float result = 0.0F;
  for (uint32_t i = 0; i < dim; ++i) {
    result += a[i] * b[i];
  }
  return result;
}

float FloatCosineSimilarity(const float* a, const float* b, uint32_t dim) {
  float dot = 0.0F;
  float norm_a = 0.0F;
  float norm_b = 0.0F;
  for (uint32_t i = 0; i < dim; ++i) {
    dot += a[i] * b[i];
    norm_a += a[i] * a[i];
    norm_b += b[i] * b[i];
  }
  norm_a = std::sqrt(norm_a);
  norm_b = std::sqrt(norm_b);
  if (norm_a < 1e-7F || norm_b < 1e-7F) {
    return 0.0F;
  }
  return dot / (norm_a * norm_b);
}

// ============================================================================
// ComputeStats
// ============================================================================

TEST(ScalarQuantizerTest, ComputeStatsSingleVector) {
  float vec[] = {1.0F, -2.0F, 3.0F, -4.0F};
  auto stats = ScalarQuantizer::ComputeStats(vec, 1, 4);
  ASSERT_EQ(stats.Dimension(), 4U);
  EXPECT_FLOAT_EQ(stats.min_vals[0], 1.0F);
  EXPECT_FLOAT_EQ(stats.max_vals[0], 1.0F);
  EXPECT_FLOAT_EQ(stats.min_vals[1], -2.0F);
  EXPECT_FLOAT_EQ(stats.max_vals[3], -4.0F);
}

TEST(ScalarQuantizerTest, ComputeStatsMultipleVectors) {
  float vecs[] = {1.0F, 2.0F, 3.0F, 4.0F, 5.0F, 6.0F};
  auto stats = ScalarQuantizer::ComputeStats(vecs, 2, 3);
  EXPECT_FLOAT_EQ(stats.min_vals[0], 1.0F);
  EXPECT_FLOAT_EQ(stats.max_vals[0], 4.0F);
  EXPECT_FLOAT_EQ(stats.min_vals[1], 2.0F);
  EXPECT_FLOAT_EQ(stats.max_vals[1], 5.0F);
  EXPECT_FLOAT_EQ(stats.min_vals[2], 3.0F);
  EXPECT_FLOAT_EQ(stats.max_vals[2], 6.0F);
}

// ============================================================================
// Quantize / Dequantize roundtrip
// ============================================================================

TEST(ScalarQuantizerTest, QuantizeDequantizeRoundtrip) {
  constexpr uint32_t kDim = 16;
  std::mt19937 rng(42);
  auto data = MakeRandomVectors(1, kDim, rng, -1.0F, 1.0F);
  auto stats = ScalarQuantizer::ComputeStats(data.data(), 1, kDim);

  std::vector<uint8_t> quantized(kDim);
  ScalarQuantizer::Quantize(data.data(), quantized.data(), kDim, stats);

  std::vector<float> restored(kDim);
  ScalarQuantizer::Dequantize(quantized.data(), restored.data(), kDim, stats);

  // Single vector: min=max per dimension, so dequantized should be exact midpoint
  // With only 1 vector, range is 0, all quantized to 128 -> restored to original
  for (uint32_t d = 0; d < kDim; ++d) {
    EXPECT_NEAR(restored[d], data[d], 0.01F) << "Dimension " << d << " mismatch";
  }
}

TEST(ScalarQuantizerTest, QuantizeDequantizeMultiVectors) {
  constexpr uint32_t kDim = 64;
  constexpr uint32_t kCount = 100;
  std::mt19937 rng(123);
  auto data = MakeRandomVectors(kCount, kDim, rng, -2.0F, 2.0F);
  auto stats = ScalarQuantizer::ComputeStats(data.data(), kCount, kDim);

  // Quantize and dequantize each vector, measure max error
  float max_error = 0.0F;
  for (uint32_t i = 0; i < kCount; ++i) {
    const float* vec = data.data() + static_cast<size_t>(i) * kDim;
    std::vector<uint8_t> quantized(kDim);
    ScalarQuantizer::Quantize(vec, quantized.data(), kDim, stats);

    std::vector<float> restored(kDim);
    ScalarQuantizer::Dequantize(quantized.data(), restored.data(), kDim, stats);

    for (uint32_t d = 0; d < kDim; ++d) {
      float err = std::abs(restored[d] - vec[d]);
      max_error = std::max(max_error, err);
    }
  }

  // With range ~4.0 and 256 levels, max quantization error ≈ 4.0/255/2 ≈ 0.008
  EXPECT_LT(max_error, 0.02F) << "Max quantization error too large";
}

// ============================================================================
// QuantizeBatch
// ============================================================================

TEST(ScalarQuantizerTest, QuantizeBatchMatchesSingle) {
  constexpr uint32_t kDim = 8;
  constexpr uint32_t kCount = 10;
  std::mt19937 rng(99);
  auto data = MakeRandomVectors(kCount, kDim, rng);
  auto stats = ScalarQuantizer::ComputeStats(data.data(), kCount, kDim);

  // Batch quantize
  std::vector<uint8_t> batch_result(static_cast<size_t>(kCount) * kDim);
  ScalarQuantizer::QuantizeBatch(data.data(), batch_result.data(), kCount, kDim, stats);

  // Single quantize
  for (uint32_t i = 0; i < kCount; ++i) {
    std::vector<uint8_t> single_result(kDim);
    ScalarQuantizer::Quantize(data.data() + static_cast<size_t>(i) * kDim, single_result.data(), kDim, stats);
    for (uint32_t d = 0; d < kDim; ++d) {
      EXPECT_EQ(batch_result[static_cast<size_t>(i) * kDim + d], single_result[d]);
    }
  }
}

// ============================================================================
// Quantized distance accuracy
// ============================================================================

TEST(ScalarQuantizerTest, QuantizedDotProductAccuracy) {
  constexpr uint32_t kDim = 128;
  constexpr uint32_t kCount = 50;
  std::mt19937 rng(777);
  auto data = MakeRandomVectors(kCount, kDim, rng, -1.0F, 1.0F);
  auto stats = ScalarQuantizer::ComputeStats(data.data(), kCount, kDim);

  // Quantize all vectors
  std::vector<uint8_t> quantized(static_cast<size_t>(kCount) * kDim);
  ScalarQuantizer::QuantizeBatch(data.data(), quantized.data(), kCount, kDim, stats);

  // Compare quantized dot product vs float32 dot product for random pairs
  float max_rel_error = 0.0F;
  int pairs_tested = 0;
  for (uint32_t i = 0; i < kCount; ++i) {
    for (uint32_t j = i + 1; j < std::min(kCount, i + 10); ++j) {
      float exact = FloatDotProduct(data.data() + static_cast<size_t>(i) * kDim,
                                    data.data() + static_cast<size_t>(j) * kDim, kDim);
      float approx =
          ScalarQuantizer::QuantizedDotProduct(quantized.data() + static_cast<size_t>(i) * kDim,
                                               quantized.data() + static_cast<size_t>(j) * kDim, kDim, stats);

      // Only check relative error for non-trivial dot products
      // Random vectors in [-1,1] with dim=128 have typical |dot| ~ 3-5
      if (std::abs(exact) > 1.0F) {
        float rel_error = std::abs(approx - exact) / std::abs(exact);
        max_rel_error = std::max(max_rel_error, rel_error);
      }
      ++pairs_tested;
    }
  }

  EXPECT_GT(pairs_tested, 100);
  // Relative error should be small (typically < 5% for SQ8)
  EXPECT_LT(max_rel_error, 0.10F) << "Quantized dot product relative error too large";
}

TEST(ScalarQuantizerTest, AsymmetricCosineAccuracy) {
  constexpr uint32_t kDim = 128;
  constexpr uint32_t kCount = 50;
  std::mt19937 rng(555);
  auto data = MakeRandomVectors(kCount, kDim, rng, -1.0F, 1.0F);
  auto stats = ScalarQuantizer::ComputeStats(data.data(), kCount, kDim);

  // Quantize all vectors
  std::vector<uint8_t> quantized(static_cast<size_t>(kCount) * kDim);
  ScalarQuantizer::QuantizeBatch(data.data(), quantized.data(), kCount, kDim, stats);

  // Compare asymmetric cosine vs float32 cosine
  float max_abs_error = 0.0F;
  for (uint32_t i = 0; i < 10; ++i) {
    const float* query = data.data() + static_cast<size_t>(i) * kDim;
    for (uint32_t j = 10; j < kCount; ++j) {
      float exact = FloatCosineSimilarity(query, data.data() + static_cast<size_t>(j) * kDim, kDim);
      float approx =
          ScalarQuantizer::AsymmetricCosine(query, quantized.data() + static_cast<size_t>(j) * kDim, kDim, stats);

      float abs_error = std::abs(approx - exact);
      max_abs_error = std::max(max_abs_error, abs_error);
    }
  }

  // Cosine similarity error should be very small (< 0.05)
  EXPECT_LT(max_abs_error, 0.05F) << "Asymmetric cosine error too large";
}

// ============================================================================
// Recall test: quantized search vs exact search
// ============================================================================

TEST(ScalarQuantizerTest, RecallAt10WithQuantization) {
  constexpr uint32_t kDim = 64;
  constexpr uint32_t kCount = 500;
  constexpr uint32_t kQueries = 20;
  constexpr uint32_t kTopK = 10;
  std::mt19937 rng(42);

  auto data = MakeRandomVectors(kCount, kDim, rng, -1.0F, 1.0F);
  auto queries = MakeRandomVectors(kQueries, kDim, rng, -1.0F, 1.0F);
  auto stats = ScalarQuantizer::ComputeStats(data.data(), kCount, kDim);

  // Quantize all database vectors
  std::vector<uint8_t> quantized(static_cast<size_t>(kCount) * kDim);
  ScalarQuantizer::QuantizeBatch(data.data(), quantized.data(), kCount, kDim, stats);

  float total_recall = 0.0F;

  for (uint32_t q = 0; q < kQueries; ++q) {
    const float* query = queries.data() + static_cast<size_t>(q) * kDim;

    // Exact top-k using float32 cosine
    std::vector<std::pair<float, uint32_t>> exact_scores;
    for (uint32_t i = 0; i < kCount; ++i) {
      float score = FloatCosineSimilarity(query, data.data() + static_cast<size_t>(i) * kDim, kDim);
      exact_scores.push_back({score, i});
    }
    std::partial_sort(exact_scores.begin(), exact_scores.begin() + kTopK, exact_scores.end(),
                      [](auto& a, auto& b) { return a.first > b.first; });

    // Approximate top-k using asymmetric cosine (float32 query, uint8 db)
    std::vector<std::pair<float, uint32_t>> approx_scores;
    for (uint32_t i = 0; i < kCount; ++i) {
      float score =
          ScalarQuantizer::AsymmetricCosine(query, quantized.data() + static_cast<size_t>(i) * kDim, kDim, stats);
      approx_scores.push_back({score, i});
    }
    std::partial_sort(approx_scores.begin(), approx_scores.begin() + kTopK, approx_scores.end(),
                      [](auto& a, auto& b) { return a.first > b.first; });

    // Compute recall: how many of the exact top-k appear in approx top-k
    std::set<uint32_t> exact_set;
    for (uint32_t k = 0; k < kTopK; ++k) {
      exact_set.insert(exact_scores[k].second);
    }

    uint32_t hits = 0;
    for (uint32_t k = 0; k < kTopK; ++k) {
      if (exact_set.count(approx_scores[k].second) > 0) {
        ++hits;
      }
    }
    total_recall += static_cast<float>(hits) / static_cast<float>(kTopK);
  }

  float avg_recall = total_recall / static_cast<float>(kQueries);

  // SQ8 should have recall@10 > 0.98 (< 2% degradation)
  EXPECT_GE(avg_recall, 0.98F) << "Recall@10 degradation exceeds 2% threshold";
}

// ============================================================================
// Edge cases
// ============================================================================

TEST(ScalarQuantizerTest, ConstantDimension) {
  // All values in a dimension are the same
  float vecs[] = {1.0F, 5.0F, 1.0F, 5.0F, 1.0F, 5.0F};
  auto stats = ScalarQuantizer::ComputeStats(vecs, 3, 2);

  // Dimension 0: all 1.0 -> range = 0
  EXPECT_FLOAT_EQ(stats.min_vals[0], 1.0F);
  EXPECT_FLOAT_EQ(stats.max_vals[0], 1.0F);

  std::vector<uint8_t> quantized(2);
  ScalarQuantizer::Quantize(vecs, quantized.data(), 2, stats);
  EXPECT_EQ(quantized[0], 128);  // Midpoint for zero-range dimension

  std::vector<float> restored(2);
  ScalarQuantizer::Dequantize(quantized.data(), restored.data(), 2, stats);
  EXPECT_NEAR(restored[0], 1.0F, 0.01F);
}

TEST(ScalarQuantizerTest, ClampOutOfRange) {
  // Need at least 2 vectors to establish a non-zero range per dimension
  float train_data[] = {0.0F, 0.0F, 10.0F, 10.0F};
  auto stats = ScalarQuantizer::ComputeStats(train_data, 2, 2);
  // dim 0: [0, 10], dim 1: [0, 10]

  // Query with values outside training range
  float out_of_range[] = {-5.0F, 15.0F};
  std::vector<uint8_t> quantized(2);
  ScalarQuantizer::Quantize(out_of_range, quantized.data(), 2, stats);

  // Should clamp to [0, 255]
  EXPECT_EQ(quantized[0], 0);
  EXPECT_EQ(quantized[1], 255);
}

}  // namespace
}  // namespace nvecd::vectors
