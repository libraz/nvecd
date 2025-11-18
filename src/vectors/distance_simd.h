/**
 * @file distance_simd.h
 * @brief SIMD dispatcher with runtime CPU detection
 *
 * Provides automatic selection of optimal SIMD implementation
 * (AVX2, NEON, or scalar fallback) based on runtime CPU detection.
 *
 * Thread-safe initialization using C++11 static initialization.
 */

#pragma once

#include <cstddef>

#include "vectors/cpu_features.h"
#include "vectors/distance_scalar.h"

// Include platform-specific SIMD implementations
#ifdef __AVX2__
#include "vectors/distance_avx2.h"
#endif

#ifdef __ARM_NEON
#include "vectors/distance_neon.h"
#endif

namespace nvecd::vectors::simd {

/**
 * @brief Function pointer types for distance operations
 */
using DotProductFunc = float (*)(const float*, const float*, size_t);
using L2NormFunc = float (*)(const float*, size_t);
using L2DistanceFunc = float (*)(const float*, const float*, size_t);

/**
 * @brief Dispatch table for distance functions
 *
 * Contains function pointers to the optimal implementation
 * selected at runtime based on CPU features.
 */
struct DistanceFunctions {
  DotProductFunc dot_product;      ///< Optimal dot product implementation
  L2NormFunc l2_norm;              ///< Optimal L2 norm implementation
  L2DistanceFunc l2_distance;      ///< Optimal L2 distance implementation
  const char* implementation_name;  ///< Name for logging (e.g., "NEON", "AVX2")
};

/**
 * @brief Get optimal SIMD implementation for current CPU
 *
 * Performs runtime CPU feature detection and returns function pointers
 * to the best available implementation. Thread-safe via static initialization.
 *
 * Selection priority:
 * 1. AVX2 (x86_64, if available)
 * 2. NEON (ARM, if available)
 * 3. Scalar (fallback, always available)
 *
 * This function is called once at first use and the result is cached.
 *
 * @return Reference to optimal function dispatch table
 */
inline const DistanceFunctions& GetOptimalImpl() {
  // Static initialization is thread-safe in C++11+
  static const DistanceFunctions impl = []() {
    CpuInfo cpu = DetectCpuFeatures();

#ifdef __AVX2__
    // AVX2 available at compile-time, check runtime support
    if (cpu.has_avx2) {
      return DistanceFunctions{DotProductAVX2, L2NormAVX2, L2DistanceAVX2,
                               "AVX2"};
    }
#endif

#ifdef __ARM_NEON
    // NEON available at compile-time, check runtime support
    if (cpu.has_neon) {
      return DistanceFunctions{DotProductNEON, L2NormNEON, L2DistanceNEON,
                               "NEON"};
    }
#endif

    // Fallback to scalar implementation
    return DistanceFunctions{DotProductScalar, L2NormScalar, L2DistanceScalar,
                             "Scalar"};
  }();

  return impl;
}

/**
 * @brief Get implementation name for logging
 *
 * Returns a string describing which SIMD implementation is active.
 * Useful for debugging and performance monitoring.
 *
 * @return Implementation name ("AVX2", "NEON", or "Scalar")
 */
inline const char* GetImplementationName() {
  return GetOptimalImpl().implementation_name;
}

}  // namespace nvecd::vectors::simd
