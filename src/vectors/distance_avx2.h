/**
 * @file distance_avx2.h
 * @brief AVX2-optimized distance functions (x86_64)
 *
 * Provides Intel AVX2 SIMD implementations of vector distance operations.
 * AVX2 uses 256-bit registers to process 8 floats simultaneously.
 *
 * Performance: ~6-8x faster than scalar for typical dimensions (256-1024).
 *
 * Requirements:
 * - AVX2 CPU support (Intel Haswell+, AMD Excavator+)
 * - Compiler flags: -mavx2 (GCC/Clang) or /arch:AVX2 (MSVC)
 */

#pragma once

#ifdef __AVX2__

#include <immintrin.h>  // AVX2 intrinsics

#include <cmath>
#include <cstddef>

namespace nvecd::vectors::simd {

/**
 * @brief Horizontal sum of 8 floats in AVX2 register
 *
 * Efficiently reduces __m256 to a single float sum.
 *
 * @param v 256-bit register containing 8 floats
 * @return Sum of all 8 lanes
 */
inline float HorizontalSumAVX2(__m256 v) {
  // v = [a0, a1, a2, a3, a4, a5, a6, a7]
  __m128 lo = _mm256_castps256_ps128(v);        // [a0, a1, a2, a3]
  __m128 hi = _mm256_extractf128_ps(v, 1);      // [a4, a5, a6, a7]
  __m128 sum128 = _mm_add_ps(lo, hi);           // [a0+a4, a1+a5, a2+a6, a3+a7]
  sum128 = _mm_hadd_ps(sum128, sum128);         // Horizontal add
  sum128 = _mm_hadd_ps(sum128, sum128);         // Final horizontal add
  return _mm_cvtss_f32(sum128);                 // Extract scalar
}

/**
 * @brief AVX2-optimized dot product
 *
 * Processes 8 floats per iteration using 256-bit AVX2 registers.
 * Uses unaligned loads for flexibility.
 *
 * @param a First vector data
 * @param b Second vector data
 * @param n Vector dimension
 * @return Dot product sum
 */
inline float DotProductAVX2(const float* a, const float* b, size_t n) {
  constexpr size_t kVecSize = 8;  // 256 bits / 32 bits per float

  __m256 sum_vec = _mm256_setzero_ps();  // Initialize accumulator to zero

  // Process 8 floats at a time
  size_t i = 0;
  for (; i + kVecSize <= n; i += kVecSize) {
    __m256 a_vec = _mm256_loadu_ps(a + i);      // Load 8 floats (unaligned)
    __m256 b_vec = _mm256_loadu_ps(b + i);
    __m256 prod = _mm256_mul_ps(a_vec, b_vec);  // Element-wise multiply
    sum_vec = _mm256_add_ps(sum_vec, prod);     // Accumulate
  }

  // Horizontal sum of 8 lanes
  float sum = HorizontalSumAVX2(sum_vec);

  // Handle remainder (scalar)
  for (; i < n; ++i) {
    sum += a[i] * b[i];
  }

  return sum;
}

/**
 * @brief AVX2-optimized L2 norm
 *
 * Computes sqrt(sum(v[i]^2)) using AVX2 SIMD.
 *
 * @param v Vector data
 * @param n Vector dimension
 * @return L2 norm (magnitude)
 */
inline float L2NormAVX2(const float* v, size_t n) {
  constexpr size_t kVecSize = 8;

  __m256 sum_vec = _mm256_setzero_ps();

  // Process 8 floats at a time
  size_t i = 0;
  for (; i + kVecSize <= n; i += kVecSize) {
    __m256 v_vec = _mm256_loadu_ps(v + i);
    __m256 sq = _mm256_mul_ps(v_vec, v_vec);  // v[i] * v[i]
    sum_vec = _mm256_add_ps(sum_vec, sq);
  }

  // Horizontal sum
  float sum_sq = HorizontalSumAVX2(sum_vec);

  // Remainder
  for (; i < n; ++i) {
    sum_sq += v[i] * v[i];
  }

  return std::sqrt(sum_sq);
}

/**
 * @brief AVX2-optimized L2 distance
 *
 * Computes sqrt(sum((a[i] - b[i])^2)) using AVX2 SIMD.
 *
 * @param a First vector data
 * @param b Second vector data
 * @param n Vector dimension
 * @return L2 (Euclidean) distance
 */
inline float L2DistanceAVX2(const float* a, const float* b, size_t n) {
  constexpr size_t kVecSize = 8;

  __m256 sum_vec = _mm256_setzero_ps();

  // Process 8 floats at a time
  size_t i = 0;
  for (; i + kVecSize <= n; i += kVecSize) {
    __m256 a_vec = _mm256_loadu_ps(a + i);
    __m256 b_vec = _mm256_loadu_ps(b + i);
    __m256 diff = _mm256_sub_ps(a_vec, b_vec);  // a[i] - b[i]
    __m256 sq = _mm256_mul_ps(diff, diff);      // diff^2
    sum_vec = _mm256_add_ps(sum_vec, sq);
  }

  // Horizontal sum
  float sum_sq = HorizontalSumAVX2(sum_vec);

  // Remainder
  for (; i < n; ++i) {
    float diff = a[i] - b[i];
    sum_sq += diff * diff;
  }

  return std::sqrt(sum_sq);
}

}  // namespace nvecd::vectors::simd

#endif  // __AVX2__
