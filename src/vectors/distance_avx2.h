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
  __m128 lo = _mm256_castps256_ps128(v);    // [a0, a1, a2, a3]
  __m128 hi = _mm256_extractf128_ps(v, 1);  // [a4, a5, a6, a7]
  __m128 sum128 = _mm_add_ps(lo, hi);       // [a0+a4, a1+a5, a2+a6, a3+a7]
  sum128 = _mm_hadd_ps(sum128, sum128);     // Horizontal add
  sum128 = _mm_hadd_ps(sum128, sum128);     // Final horizontal add
  return _mm_cvtss_f32(sum128);             // Extract scalar
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
  constexpr size_t kVecSize = 8;   // 256 bits / 32 bits per float
  constexpr size_t kUnroll = 4;    // 4 accumulators to hide FMA latency
  constexpr size_t kStride = kVecSize * kUnroll;  // 32 floats per iteration

  // 4 independent accumulators for instruction-level parallelism
  __m256 sum0 = _mm256_setzero_ps();
  __m256 sum1 = _mm256_setzero_ps();
  __m256 sum2 = _mm256_setzero_ps();
  __m256 sum3 = _mm256_setzero_ps();

  // Process 32 floats at a time (4 AVX2 registers x 8 floats)
  size_t i = 0;
  for (; i + kStride <= n; i += kStride) {
    sum0 = _mm256_add_ps(sum0, _mm256_mul_ps(_mm256_loadu_ps(a + i),      _mm256_loadu_ps(b + i)));
    sum1 = _mm256_add_ps(sum1, _mm256_mul_ps(_mm256_loadu_ps(a + i + 8),  _mm256_loadu_ps(b + i + 8)));
    sum2 = _mm256_add_ps(sum2, _mm256_mul_ps(_mm256_loadu_ps(a + i + 16), _mm256_loadu_ps(b + i + 16)));
    sum3 = _mm256_add_ps(sum3, _mm256_mul_ps(_mm256_loadu_ps(a + i + 24), _mm256_loadu_ps(b + i + 24)));
  }

  // Process remaining 8-float blocks
  for (; i + kVecSize <= n; i += kVecSize) {
    sum0 = _mm256_add_ps(sum0, _mm256_mul_ps(_mm256_loadu_ps(a + i), _mm256_loadu_ps(b + i)));
  }

  // Combine accumulators
  sum0 = _mm256_add_ps(sum0, sum1);
  sum2 = _mm256_add_ps(sum2, sum3);
  sum0 = _mm256_add_ps(sum0, sum2);

  float sum = HorizontalSumAVX2(sum0);

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
  constexpr size_t kStride = kVecSize * 4;

  __m256 sum0 = _mm256_setzero_ps();
  __m256 sum1 = _mm256_setzero_ps();
  __m256 sum2 = _mm256_setzero_ps();
  __m256 sum3 = _mm256_setzero_ps();

  size_t i = 0;
  for (; i + kStride <= n; i += kStride) {
    __m256 v0 = _mm256_loadu_ps(v + i);
    __m256 v1 = _mm256_loadu_ps(v + i + 8);
    __m256 v2 = _mm256_loadu_ps(v + i + 16);
    __m256 v3 = _mm256_loadu_ps(v + i + 24);
    sum0 = _mm256_add_ps(sum0, _mm256_mul_ps(v0, v0));
    sum1 = _mm256_add_ps(sum1, _mm256_mul_ps(v1, v1));
    sum2 = _mm256_add_ps(sum2, _mm256_mul_ps(v2, v2));
    sum3 = _mm256_add_ps(sum3, _mm256_mul_ps(v3, v3));
  }

  for (; i + kVecSize <= n; i += kVecSize) {
    __m256 v_vec = _mm256_loadu_ps(v + i);
    sum0 = _mm256_add_ps(sum0, _mm256_mul_ps(v_vec, v_vec));
  }

  sum0 = _mm256_add_ps(sum0, sum1);
  sum2 = _mm256_add_ps(sum2, sum3);
  sum0 = _mm256_add_ps(sum0, sum2);
  float sum_sq = HorizontalSumAVX2(sum0);

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
  constexpr size_t kStride = kVecSize * 4;

  __m256 sum0 = _mm256_setzero_ps();
  __m256 sum1 = _mm256_setzero_ps();
  __m256 sum2 = _mm256_setzero_ps();
  __m256 sum3 = _mm256_setzero_ps();

  size_t i = 0;
  for (; i + kStride <= n; i += kStride) {
    __m256 d0 = _mm256_sub_ps(_mm256_loadu_ps(a + i),      _mm256_loadu_ps(b + i));
    __m256 d1 = _mm256_sub_ps(_mm256_loadu_ps(a + i + 8),  _mm256_loadu_ps(b + i + 8));
    __m256 d2 = _mm256_sub_ps(_mm256_loadu_ps(a + i + 16), _mm256_loadu_ps(b + i + 16));
    __m256 d3 = _mm256_sub_ps(_mm256_loadu_ps(a + i + 24), _mm256_loadu_ps(b + i + 24));
    sum0 = _mm256_add_ps(sum0, _mm256_mul_ps(d0, d0));
    sum1 = _mm256_add_ps(sum1, _mm256_mul_ps(d1, d1));
    sum2 = _mm256_add_ps(sum2, _mm256_mul_ps(d2, d2));
    sum3 = _mm256_add_ps(sum3, _mm256_mul_ps(d3, d3));
  }

  for (; i + kVecSize <= n; i += kVecSize) {
    __m256 diff = _mm256_sub_ps(_mm256_loadu_ps(a + i), _mm256_loadu_ps(b + i));
    sum0 = _mm256_add_ps(sum0, _mm256_mul_ps(diff, diff));
  }

  sum0 = _mm256_add_ps(sum0, sum1);
  sum2 = _mm256_add_ps(sum2, sum3);
  sum0 = _mm256_add_ps(sum0, sum2);
  float sum_sq = HorizontalSumAVX2(sum0);

  for (; i < n; ++i) {
    float diff = a[i] - b[i];
    sum_sq += diff * diff;
  }

  return std::sqrt(sum_sq);
}

}  // namespace nvecd::vectors::simd

#endif  // __AVX2__
