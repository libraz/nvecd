/**
 * @file distance_functions_test.cpp
 * @brief Unit tests for high-level distance/similarity functions in distance.h
 */

#include "vectors/distance.h"

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

namespace nvecd::vectors {

// ============================================================================
// CosineSimilarity
// ============================================================================

TEST(DistanceFunctionsTest, CosineSimilarity_IdenticalVectors) {
  std::vector<float> v1 = {1.0F, 0.0F, 0.0F};
  float sim = CosineSimilarity(v1, v1);
  EXPECT_NEAR(sim, 1.0F, 1e-6F);
}

TEST(DistanceFunctionsTest, CosineSimilarity_OrthogonalVectors) {
  std::vector<float> v1 = {1.0F, 0.0F, 0.0F};
  std::vector<float> v2 = {0.0F, 1.0F, 0.0F};
  float sim = CosineSimilarity(v1, v2);
  EXPECT_NEAR(sim, 0.0F, 1e-6F);
}

TEST(DistanceFunctionsTest, CosineSimilarity_OppositeVectors) {
  std::vector<float> v1 = {1.0F, 0.0F, 0.0F};
  std::vector<float> v2 = {-1.0F, 0.0F, 0.0F};
  float sim = CosineSimilarity(v1, v2);
  EXPECT_NEAR(sim, -1.0F, 1e-6F);
}

TEST(DistanceFunctionsTest, CosineSimilarity_KnownAngle) {
  // Two vectors at 60 degrees: cos(60) = 0.5
  std::vector<float> v1 = {1.0F, 0.0F};
  std::vector<float> v2 = {0.5F, std::sqrt(3.0F) / 2.0F};
  float sim = CosineSimilarity(v1, v2);
  EXPECT_NEAR(sim, 0.5F, 1e-5F);
}

TEST(DistanceFunctionsTest, CosineSimilarity_DifferentMagnitudes) {
  std::vector<float> v1 = {1.0F, 0.0F, 0.0F};
  std::vector<float> v2 = {100.0F, 0.0F, 0.0F};
  float sim = CosineSimilarity(v1, v2);
  EXPECT_NEAR(sim, 1.0F, 1e-6F);
}

TEST(DistanceFunctionsTest, CosineSimilarity_ZeroVector_ReturnsZero) {
  std::vector<float> v1 = {1.0F, 2.0F, 3.0F};
  std::vector<float> v2 = {0.0F, 0.0F, 0.0F};
  float sim = CosineSimilarity(v1, v2);
  EXPECT_NEAR(sim, 0.0F, 1e-6F);
}

TEST(DistanceFunctionsTest, CosineSimilarity_BothZeroVectors_ReturnsZero) {
  std::vector<float> v1 = {0.0F, 0.0F, 0.0F};
  std::vector<float> v2 = {0.0F, 0.0F, 0.0F};
  float sim = CosineSimilarity(v1, v2);
  EXPECT_NEAR(sim, 0.0F, 1e-6F);
}

TEST(DistanceFunctionsTest, CosineSimilarity_DimensionMismatch_ReturnsZero) {
  std::vector<float> v1 = {1.0F, 2.0F};
  std::vector<float> v2 = {1.0F, 2.0F, 3.0F};
  float sim = CosineSimilarity(v1, v2);
  EXPECT_NEAR(sim, 0.0F, 1e-6F);
}

TEST(DistanceFunctionsTest, CosineSimilarity_EmptyVectors_ReturnsZero) {
  std::vector<float> v1;
  std::vector<float> v2;
  float sim = CosineSimilarity(v1, v2);
  EXPECT_NEAR(sim, 0.0F, 1e-6F);
}

TEST(DistanceFunctionsTest, CosineSimilarity_SingleElement) {
  std::vector<float> v1 = {3.0F};
  std::vector<float> v2 = {5.0F};
  float sim = CosineSimilarity(v1, v2);
  EXPECT_NEAR(sim, 1.0F, 1e-6F);
}

// ============================================================================
// DotProduct
// ============================================================================

TEST(DistanceFunctionsTest, DotProduct_KnownValues) {
  std::vector<float> v1 = {1.0F, 2.0F, 3.0F};
  std::vector<float> v2 = {4.0F, 5.0F, 6.0F};
  float dot = DotProduct(v1, v2);
  // 1*4 + 2*5 + 3*6 = 4 + 10 + 18 = 32
  EXPECT_NEAR(dot, 32.0F, 1e-5F);
}

TEST(DistanceFunctionsTest, DotProduct_OrthogonalVectors) {
  std::vector<float> v1 = {1.0F, 0.0F, 0.0F};
  std::vector<float> v2 = {0.0F, 1.0F, 0.0F};
  float dot = DotProduct(v1, v2);
  EXPECT_NEAR(dot, 0.0F, 1e-6F);
}

TEST(DistanceFunctionsTest, DotProduct_IdenticalUnitVectors) {
  std::vector<float> v1 = {1.0F, 0.0F, 0.0F};
  float dot = DotProduct(v1, v1);
  EXPECT_NEAR(dot, 1.0F, 1e-6F);
}

TEST(DistanceFunctionsTest, DotProduct_NegativeValues) {
  std::vector<float> v1 = {1.0F, -2.0F, 3.0F};
  std::vector<float> v2 = {-4.0F, 5.0F, -6.0F};
  float dot = DotProduct(v1, v2);
  // 1*(-4) + (-2)*5 + 3*(-6) = -4 - 10 - 18 = -32
  EXPECT_NEAR(dot, -32.0F, 1e-5F);
}

TEST(DistanceFunctionsTest, DotProduct_ZeroVector) {
  std::vector<float> v1 = {1.0F, 2.0F, 3.0F};
  std::vector<float> v2 = {0.0F, 0.0F, 0.0F};
  float dot = DotProduct(v1, v2);
  EXPECT_NEAR(dot, 0.0F, 1e-6F);
}

TEST(DistanceFunctionsTest, DotProduct_DimensionMismatch_ReturnsZero) {
  std::vector<float> v1 = {1.0F, 2.0F};
  std::vector<float> v2 = {1.0F, 2.0F, 3.0F};
  float dot = DotProduct(v1, v2);
  EXPECT_NEAR(dot, 0.0F, 1e-6F);
}

TEST(DistanceFunctionsTest, DotProduct_EmptyVectors_ReturnsZero) {
  std::vector<float> v1;
  std::vector<float> v2;
  float dot = DotProduct(v1, v2);
  EXPECT_NEAR(dot, 0.0F, 1e-6F);
}

TEST(DistanceFunctionsTest, DotProduct_SingleElement) {
  std::vector<float> v1 = {3.0F};
  std::vector<float> v2 = {7.0F};
  float dot = DotProduct(v1, v2);
  EXPECT_NEAR(dot, 21.0F, 1e-6F);
}

// ============================================================================
// L2Distance
// ============================================================================

TEST(DistanceFunctionsTest, L2Distance_IdenticalVectors_Zero) {
  std::vector<float> v1 = {1.0F, 2.0F, 3.0F};
  float dist = L2Distance(v1, v1);
  EXPECT_NEAR(dist, 0.0F, 1e-6F);
}

TEST(DistanceFunctionsTest, L2Distance_KnownValues) {
  std::vector<float> v1 = {0.0F, 0.0F, 0.0F};
  std::vector<float> v2 = {3.0F, 4.0F, 0.0F};
  float dist = L2Distance(v1, v2);
  // sqrt(9 + 16 + 0) = 5
  EXPECT_NEAR(dist, 5.0F, 1e-5F);
}

TEST(DistanceFunctionsTest, L2Distance_UnitVectors) {
  std::vector<float> v1 = {1.0F, 0.0F};
  std::vector<float> v2 = {0.0F, 1.0F};
  float dist = L2Distance(v1, v2);
  // sqrt(1 + 1) = sqrt(2)
  EXPECT_NEAR(dist, std::sqrt(2.0F), 1e-5F);
}

TEST(DistanceFunctionsTest, L2Distance_DimensionMismatch_ReturnsZero) {
  std::vector<float> v1 = {1.0F, 2.0F};
  std::vector<float> v2 = {1.0F, 2.0F, 3.0F};
  float dist = L2Distance(v1, v2);
  EXPECT_NEAR(dist, 0.0F, 1e-6F);
}

TEST(DistanceFunctionsTest, L2Distance_EmptyVectors_ReturnsZero) {
  std::vector<float> v1;
  std::vector<float> v2;
  float dist = L2Distance(v1, v2);
  EXPECT_NEAR(dist, 0.0F, 1e-6F);
}

TEST(DistanceFunctionsTest, L2Distance_SingleElement) {
  std::vector<float> v1 = {0.0F};
  std::vector<float> v2 = {5.0F};
  float dist = L2Distance(v1, v2);
  EXPECT_NEAR(dist, 5.0F, 1e-5F);
}

TEST(DistanceFunctionsTest, L2Distance_Symmetric) {
  std::vector<float> v1 = {1.0F, 3.0F, 5.0F};
  std::vector<float> v2 = {2.0F, 4.0F, 6.0F};
  EXPECT_NEAR(L2Distance(v1, v2), L2Distance(v2, v1), 1e-6F);
}

// ============================================================================
// L2Norm
// ============================================================================

TEST(DistanceFunctionsTest, L2Norm_UnitVector) {
  std::vector<float> v = {1.0F, 0.0F, 0.0F};
  EXPECT_NEAR(L2Norm(v), 1.0F, 1e-6F);
}

TEST(DistanceFunctionsTest, L2Norm_KnownValue) {
  std::vector<float> v = {3.0F, 4.0F};
  EXPECT_NEAR(L2Norm(v), 5.0F, 1e-5F);
}

TEST(DistanceFunctionsTest, L2Norm_ZeroVector) {
  std::vector<float> v = {0.0F, 0.0F, 0.0F};
  EXPECT_NEAR(L2Norm(v), 0.0F, 1e-6F);
}

TEST(DistanceFunctionsTest, L2Norm_EmptyVector) {
  std::vector<float> v;
  EXPECT_NEAR(L2Norm(v), 0.0F, 1e-6F);
}

// ============================================================================
// CosineSimilarityPreNorm
// ============================================================================

TEST(DistanceFunctionsTest, CosineSimilarityPreNorm_IdenticalVectors) {
  std::vector<float> v = {1.0F, 2.0F, 3.0F};
  float norm = L2Norm(v);
  float sim = CosineSimilarityPreNorm(v.data(), v.data(), v.size(), norm, norm);
  EXPECT_NEAR(sim, 1.0F, 1e-5F);
}

TEST(DistanceFunctionsTest, CosineSimilarityPreNorm_OrthogonalVectors) {
  std::vector<float> v1 = {1.0F, 0.0F, 0.0F};
  std::vector<float> v2 = {0.0F, 1.0F, 0.0F};
  float norm1 = L2Norm(v1);
  float norm2 = L2Norm(v2);
  float sim = CosineSimilarityPreNorm(v1.data(), v2.data(), v1.size(), norm1, norm2);
  EXPECT_NEAR(sim, 0.0F, 1e-6F);
}

TEST(DistanceFunctionsTest, CosineSimilarityPreNorm_MatchesCosineSimilarity) {
  std::vector<float> v1 = {1.0F, 2.0F, 3.0F};
  std::vector<float> v2 = {4.0F, 5.0F, 6.0F};
  float norm1 = L2Norm(v1);
  float norm2 = L2Norm(v2);

  float sim_prenorm = CosineSimilarityPreNorm(v1.data(), v2.data(), v1.size(), norm1, norm2);
  float sim_regular = CosineSimilarity(v1, v2);
  EXPECT_NEAR(sim_prenorm, sim_regular, 1e-5F);
}

TEST(DistanceFunctionsTest, CosineSimilarityPreNorm_ZeroNorm_ReturnsZero) {
  std::vector<float> v1 = {1.0F, 2.0F, 3.0F};
  std::vector<float> v2 = {0.0F, 0.0F, 0.0F};
  float sim = CosineSimilarityPreNorm(v1.data(), v2.data(), v1.size(), L2Norm(v1), 0.0F);
  EXPECT_NEAR(sim, 0.0F, 1e-6F);
}

// ============================================================================
// Normalize
// ============================================================================

TEST(DistanceFunctionsTest, Normalize_ProducesUnitVector) {
  std::vector<float> v = {3.0F, 4.0F, 0.0F};
  ASSERT_TRUE(Normalize(v));
  EXPECT_NEAR(L2Norm(v), 1.0F, 1e-5F);
}

TEST(DistanceFunctionsTest, Normalize_ZeroVector_ReturnsFalse) {
  std::vector<float> v = {0.0F, 0.0F, 0.0F};
  EXPECT_FALSE(Normalize(v));
}

TEST(DistanceFunctionsTest, NormalizedCopy_PreservesOriginal) {
  std::vector<float> v = {3.0F, 4.0F};
  auto normalized = NormalizedCopy(v);
  // Original unchanged
  EXPECT_NEAR(v[0], 3.0F, 1e-6F);
  EXPECT_NEAR(v[1], 4.0F, 1e-6F);
  // Copy is normalized
  EXPECT_NEAR(L2Norm(normalized), 1.0F, 1e-5F);
}

TEST(DistanceFunctionsTest, NormalizedCopy_ZeroVector_ReturnsEmpty) {
  std::vector<float> v = {0.0F, 0.0F};
  auto normalized = NormalizedCopy(v);
  EXPECT_TRUE(normalized.empty());
}

// ============================================================================
// Large dimension stress test
// ============================================================================

TEST(DistanceFunctionsTest, CosineSimilarity_LargeDimension) {
  // 1024-dimensional vectors
  std::vector<float> v1(1024, 1.0F);
  std::vector<float> v2(1024, 1.0F);
  float sim = CosineSimilarity(v1, v2);
  EXPECT_NEAR(sim, 1.0F, 1e-5F);
}

TEST(DistanceFunctionsTest, DotProduct_LargeDimension) {
  std::vector<float> v1(1024, 1.0F);
  std::vector<float> v2(1024, 2.0F);
  float dot = DotProduct(v1, v2);
  // 1024 * (1 * 2) = 2048
  EXPECT_NEAR(dot, 2048.0F, 1e-2F);
}

TEST(DistanceFunctionsTest, L2Distance_LargeDimension) {
  std::vector<float> v1(1024, 0.0F);
  std::vector<float> v2(1024, 1.0F);
  float dist = L2Distance(v1, v2);
  // sqrt(1024 * 1^2) = sqrt(1024) = 32
  EXPECT_NEAR(dist, 32.0F, 1e-3F);
}

}  // namespace nvecd::vectors
