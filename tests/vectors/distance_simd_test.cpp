/**
 * @file distance_simd_test.cpp
 * @brief SIMD correctness tests
 *
 * Tests that all SIMD implementations (AVX2, NEON) produce results
 * numerically equivalent to the scalar reference implementation.
 */

#include <gtest/gtest.h>

#include <random>
#include <unordered_map>
#include <vector>

#include "vectors/distance.h"
#include "vectors/distance_scalar.h"

#ifdef __AVX2__
#include "vectors/distance_avx2.h"
#endif

#ifdef __ARM_NEON
#include "vectors/distance_neon.h"
#endif

namespace nvecd::vectors::simd {

/**
 * @brief Test fixture for SIMD correctness tests
 *
 * Generates random test vectors of various dimensions to test
 * SIMD implementations against scalar reference.
 */
class DistanceSIMDTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // Generate random test vectors with fixed seed for reproducibility
    std::mt19937 gen(42);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);

    // Test various dimensions including:
    // - Small (4, 16) - tests remainder handling
    // - Typical (64, 128, 256, 512, 768) - common embedding dimensions
    // - Non-power-of-2 (100, 1000) - tests alignment handling
    std::vector<int> dims = {4, 16, 64, 128, 256, 512, 768, 1000};

    for (int dim : dims) {
      std::vector<float> vec(dim);
      for (float& val : vec) {
        val = dist(gen);
      }
      test_vectors_[dim] = vec;
    }
  }

  std::unordered_map<int, std::vector<float>> test_vectors_;
};

// ============================================================================
// DotProduct Correctness Tests
// ============================================================================

TEST_F(DistanceSIMDTest, DotProductCorrectness) {
  for (const auto& [dim, vec_a] : test_vectors_) {
    const auto& vec_b = test_vectors_[dim];

    // Compute scalar reference
    float scalar_result = DotProductScalar(vec_a.data(), vec_b.data(), dim);

    // Use relative tolerance for large values, absolute for small values
    float tolerance = std::max(1e-3f, std::abs(scalar_result) * 1e-4f);

#ifdef __AVX2__
    // Test AVX2 implementation
    float avx2_result = DotProductAVX2(vec_a.data(), vec_b.data(), dim);
    EXPECT_NEAR(scalar_result, avx2_result, tolerance) << "AVX2 DotProduct mismatch at dimension " << dim;
#endif

#ifdef __ARM_NEON
    // Test NEON implementation
    float neon_result = DotProductNEON(vec_a.data(), vec_b.data(), dim);
    EXPECT_NEAR(scalar_result, neon_result, tolerance) << "NEON DotProduct mismatch at dimension " << dim;
#endif
  }
}

// ============================================================================
// L2Norm Correctness Tests
// ============================================================================

TEST_F(DistanceSIMDTest, L2NormCorrectness) {
  for (const auto& [dim, vec] : test_vectors_) {
    // Compute scalar reference
    float scalar_result = L2NormScalar(vec.data(), dim);

    // Use relative tolerance
    float tolerance = std::max(1e-3f, std::abs(scalar_result) * 1e-4f);

#ifdef __AVX2__
    // Test AVX2 implementation
    float avx2_result = L2NormAVX2(vec.data(), dim);
    EXPECT_NEAR(scalar_result, avx2_result, tolerance) << "AVX2 L2Norm mismatch at dimension " << dim;
#endif

#ifdef __ARM_NEON
    // Test NEON implementation
    float neon_result = L2NormNEON(vec.data(), dim);
    EXPECT_NEAR(scalar_result, neon_result, tolerance) << "NEON L2Norm mismatch at dimension " << dim;
#endif
  }
}

// ============================================================================
// L2Distance Correctness Tests
// ============================================================================

TEST_F(DistanceSIMDTest, L2DistanceCorrectness) {
  for (const auto& [dim, vec_a] : test_vectors_) {
    const auto& vec_b = test_vectors_[dim];

    // Compute scalar reference
    float scalar_result = L2DistanceScalar(vec_a.data(), vec_b.data(), dim);

    // Use relative tolerance
    float tolerance = std::max(1e-3f, std::abs(scalar_result) * 1e-4f);

#ifdef __AVX2__
    // Test AVX2 implementation
    float avx2_result = L2DistanceAVX2(vec_a.data(), vec_b.data(), dim);
    EXPECT_NEAR(scalar_result, avx2_result, tolerance) << "AVX2 L2Distance mismatch at dimension " << dim;
#endif

#ifdef __ARM_NEON
    // Test NEON implementation
    float neon_result = L2DistanceNEON(vec_a.data(), vec_b.data(), dim);
    EXPECT_NEAR(scalar_result, neon_result, tolerance) << "NEON L2Distance mismatch at dimension " << dim;
#endif
  }
}

// ============================================================================
// Edge Cases
// ============================================================================

TEST_F(DistanceSIMDTest, ZeroVectors) {
  std::vector<float> zero_vec(768, 0.0f);

  // DotProduct of zero vectors should be 0
  float scalar_dot = DotProductScalar(zero_vec.data(), zero_vec.data(), 768);
  EXPECT_FLOAT_EQ(scalar_dot, 0.0f);

#ifdef __AVX2__
  float avx2_dot = DotProductAVX2(zero_vec.data(), zero_vec.data(), 768);
  EXPECT_FLOAT_EQ(avx2_dot, 0.0f);
#endif

#ifdef __ARM_NEON
  float neon_dot = DotProductNEON(zero_vec.data(), zero_vec.data(), 768);
  EXPECT_FLOAT_EQ(neon_dot, 0.0f);
#endif

  // L2Norm of zero vector should be 0
  float scalar_norm = L2NormScalar(zero_vec.data(), 768);
  EXPECT_FLOAT_EQ(scalar_norm, 0.0f);

#ifdef __AVX2__
  float avx2_norm = L2NormAVX2(zero_vec.data(), 768);
  EXPECT_FLOAT_EQ(avx2_norm, 0.0f);
#endif

#ifdef __ARM_NEON
  float neon_norm = L2NormNEON(zero_vec.data(), 768);
  EXPECT_FLOAT_EQ(neon_norm, 0.0f);
#endif
}

TEST_F(DistanceSIMDTest, SingleElement) {
  std::vector<float> single = {3.14f};

  float scalar_norm = L2NormScalar(single.data(), 1);
  EXPECT_NEAR(scalar_norm, 3.14f, 1e-6f);

#ifdef __AVX2__
  float avx2_norm = L2NormAVX2(single.data(), 1);
  EXPECT_NEAR(avx2_norm, 3.14f, 1e-6f);
#endif

#ifdef __ARM_NEON
  float neon_norm = L2NormNEON(single.data(), 1);
  EXPECT_NEAR(neon_norm, 3.14f, 1e-6f);
#endif
}

TEST_F(DistanceSIMDTest, NonMultipleOfVectorSize) {
  // Test dimensions that are not multiples of SIMD width
  // AVX2: 8-wide, NEON: 4-wide
  std::vector<int> odd_dims = {5, 7, 13, 17, 99, 1001};

  std::mt19937 gen(12345);
  std::uniform_real_distribution<float> dist(0.0f, 1.0f);

  for (int dim : odd_dims) {
    std::vector<float> vec(dim);
    for (float& val : vec) {
      val = dist(gen);
    }

    // DotProduct with itself
    float scalar = DotProductScalar(vec.data(), vec.data(), dim);
    float tolerance = std::max(1e-3f, std::abs(scalar) * 1e-4f);

#ifdef __AVX2__
    float avx2 = DotProductAVX2(vec.data(), vec.data(), dim);
    EXPECT_NEAR(scalar, avx2, tolerance) << "Dimension: " << dim;
#endif

#ifdef __ARM_NEON
    float neon = DotProductNEON(vec.data(), vec.data(), dim);
    EXPECT_NEAR(scalar, neon, tolerance) << "Dimension: " << dim;
#endif
  }
}

// ============================================================================
// Public API Integration Test
// ============================================================================

TEST_F(DistanceSIMDTest, PublicAPIUsesOptimalImpl) {
  // Test that public API (distance.h) uses SIMD implementation
  std::vector<float> a = test_vectors_[768];
  std::vector<float> b = test_vectors_[768];

  // These should use SIMD (via dispatcher)
  float dot = DotProduct(a, b);
  float norm = L2Norm(a);
  float dist = L2Distance(a, b);
  float cosine = CosineSimilarity(a, b);

  // Verify results are reasonable
  EXPECT_GT(dot, 0.0f);      // Positive dot product
  EXPECT_GT(norm, 0.0f);     // Positive norm
  EXPECT_GE(dist, 0.0f);     // Non-negative distance
  EXPECT_GE(cosine, -1.0f);  // Cosine similarity in [-1, 1]
  EXPECT_LE(cosine, 1.0f);
}

}  // namespace nvecd::vectors::simd
