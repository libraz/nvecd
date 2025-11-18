/**
 * @file distance_neon.h
 * @brief NEON-optimized distance functions (ARM)
 *
 * Provides ARM NEON SIMD implementations of vector distance operations.
 * NEON is the baseline SIMD extension for AArch64 (ARM64).
 *
 * Performance: ~3-4x faster than scalar for typical dimensions (256-1024).
 *
 * Requirements:
 * - NEON support (baseline for AArch64, optional for ARMv7)
 * - Compiler flags: Automatic for AArch64, -mfpu=neon for ARMv7
 */

#pragma once

#ifdef __ARM_NEON

#include <arm_neon.h>  // NEON intrinsics

#include <cmath>
#include <cstddef>

namespace nvecd::vectors::simd {

/**
 * @brief NEON-optimized dot product
 *
 * Processes 4 floats per iteration using 128-bit NEON registers.
 * Uses fused multiply-add (VMLA) for better performance.
 *
 * @param a First vector data
 * @param b Second vector data
 * @param n Vector dimension
 * @return Dot product sum
 */
inline float DotProductNEON(const float* a, const float* b, size_t n) {
  constexpr size_t kVecSize = 4;  // 128 bits / 32 bits per float

  float32x4_t sum_vec = vdupq_n_f32(0.0F);  // Initialize accumulator to zero

  // Process 4 floats at a time
  size_t vec_idx = 0;
  for (; vec_idx + kVecSize <= n; vec_idx += kVecSize) {
    float32x4_t a_vec =
        vld1q_f32(a + vec_idx);  // Load 4 floats from a  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    float32x4_t b_vec =
        vld1q_f32(b + vec_idx);  // Load 4 floats from b  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    sum_vec = vmlaq_f32(sum_vec, a_vec, b_vec);  // sum += a * b (fused)
  }

  // Horizontal sum of 4 lanes (AArch64 has reduction instruction)
#if defined(__aarch64__)
  float sum = vaddvq_f32(sum_vec);  // AArch64: Single instruction reduction
#else
  // ARMv7: Manual pairwise reduction
  float32x2_t sum_lo = vget_low_f32(sum_vec);
  float32x2_t sum_hi = vget_high_f32(sum_vec);
  float32x2_t sum_pair = vadd_f32(sum_lo, sum_hi);
  float sum = vget_lane_f32(sum_pair, 0) + vget_lane_f32(sum_pair, 1);
#endif

  // Handle remainder (scalar)
  for (; vec_idx < n; ++vec_idx) {
    sum += a[vec_idx] * b[vec_idx];  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  }

  return sum;
}

/**
 * @brief NEON-optimized L2 norm
 *
 * Computes sqrt(sum(v[i]^2)) using NEON SIMD.
 *
 * @param v Vector data
 * @param n Vector dimension
 * @return L2 norm (magnitude)
 */
inline float L2NormNEON(const float* vec_data, size_t n) {
  constexpr size_t kVecSize = 4;

  float32x4_t sum_vec = vdupq_n_f32(0.0F);

  // Process 4 floats at a time
  size_t vec_idx = 0;
  for (; vec_idx + kVecSize <= n; vec_idx += kVecSize) {
    float32x4_t v_vec = vld1q_f32(vec_data + vec_idx);  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    sum_vec = vmlaq_f32(sum_vec, v_vec, v_vec);         // sum += v * v
  }

  // Horizontal sum
#if defined(__aarch64__)
  float sum_sq = vaddvq_f32(sum_vec);
#else
  float32x2_t sum_lo = vget_low_f32(sum_vec);
  float32x2_t sum_hi = vget_high_f32(sum_vec);
  float32x2_t sum_pair = vadd_f32(sum_lo, sum_hi);
  float sum_sq = vget_lane_f32(sum_pair, 0) + vget_lane_f32(sum_pair, 1);
#endif

  // Remainder
  for (; vec_idx < n; ++vec_idx) {
    sum_sq += vec_data[vec_idx] * vec_data[vec_idx];  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  }

  return std::sqrt(sum_sq);
}

/**
 * @brief NEON-optimized L2 distance
 *
 * Computes sqrt(sum((a[i] - b[i])^2)) using NEON SIMD.
 *
 * @param a First vector data
 * @param b Second vector data
 * @param n Vector dimension
 * @return L2 (Euclidean) distance
 */
inline float L2DistanceNEON(const float* a, const float* b, size_t n) {
  constexpr size_t kVecSize = 4;

  float32x4_t sum_vec = vdupq_n_f32(0.0F);

  // Process 4 floats at a time
  size_t vec_idx = 0;
  for (; vec_idx + kVecSize <= n; vec_idx += kVecSize) {
    float32x4_t a_vec = vld1q_f32(a + vec_idx);  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    float32x4_t b_vec = vld1q_f32(b + vec_idx);  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    float32x4_t diff = vsubq_f32(a_vec, b_vec);  // a - b
    sum_vec = vmlaq_f32(sum_vec, diff, diff);    // sum += diff * diff
  }

  // Horizontal sum
#if defined(__aarch64__)
  float sum_sq = vaddvq_f32(sum_vec);
#else
  float32x2_t sum_lo = vget_low_f32(sum_vec);
  float32x2_t sum_hi = vget_high_f32(sum_vec);
  float32x2_t sum_pair = vadd_f32(sum_lo, sum_hi);
  float sum_sq = vget_lane_f32(sum_pair, 0) + vget_lane_f32(sum_pair, 1);
#endif

  // Remainder
  for (; vec_idx < n; ++vec_idx) {
    float diff = a[vec_idx] - b[vec_idx];  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    sum_sq += diff * diff;
  }

  return std::sqrt(sum_sq);
}

}  // namespace nvecd::vectors::simd

#endif  // __ARM_NEON
