/**
 * @file distance_scalar.h
 * @brief Scalar (non-SIMD) reference implementations
 *
 * These are fallback implementations used when SIMD is not available,
 * and serve as reference for correctness testing.
 *
 * All functions take raw pointers for flexibility in SIMD dispatch.
 */

#pragma once

#include <cmath>
#include <cstddef>

namespace nvecd::vectors::simd {

/**
 * @brief Scalar dot product implementation
 *
 * Computes sum(a[i] * b[i]) for i in [0, n).
 *
 * @param a First vector data
 * @param b Second vector data
 * @param n Vector dimension
 * @return Dot product sum
 */
inline float DotProductScalar(const float* a, const float* b, size_t n) {
  float sum = 0.0F;
  for (size_t i = 0; i < n; ++i) {
    sum += a[i] * b[i];  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  }
  return sum;
}

/**
 * @brief Scalar L2 norm implementation
 *
 * Computes sqrt(sum(v[i]^2)) for i in [0, n).
 *
 * @param v Vector data
 * @param n Vector dimension
 * @return L2 norm (magnitude)
 */
inline float L2NormScalar(const float* v, size_t n) {
  float sum_sq = 0.0F;
  for (size_t i = 0; i < n; ++i) {
    sum_sq += v[i] * v[i];  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  }
  return std::sqrt(sum_sq);
}

/**
 * @brief Scalar L2 distance implementation
 *
 * Computes sqrt(sum((a[i] - b[i])^2)) for i in [0, n).
 *
 * @param a First vector data
 * @param b Second vector data
 * @param n Vector dimension
 * @return L2 (Euclidean) distance
 */
inline float L2DistanceScalar(const float* a, const float* b, size_t n) {
  float sum_sq = 0.0F;
  for (size_t i = 0; i < n; ++i) {
    float diff = a[i] - b[i];  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    sum_sq += diff * diff;
  }
  return std::sqrt(sum_sq);
}

}  // namespace nvecd::vectors::simd
