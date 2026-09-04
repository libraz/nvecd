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

// The ISA-specific kernels are deliberately NOT included here. Selecting one is
// a compile-time decision that depends on -mavx2, which CMake applies only to
// the nvecd_vectors target; a header that branched on __AVX2__ would compile a
// different body in every other target. The selection therefore lives in
// distance_simd.cpp, which is part of that target, and this header only
// declares the result.

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
  DotProductFunc dot_product;       ///< Optimal dot product implementation
  L2NormFunc l2_norm;               ///< Optimal L2 norm implementation
  L2DistanceFunc l2_distance;       ///< Optimal L2 distance implementation
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
const DistanceFunctions& GetOptimalImpl();

/**
 * @brief Get implementation name for logging
 *
 * Returns a string describing which SIMD implementation is active.
 * Useful for debugging and performance monitoring.
 *
 * @return Implementation name ("AVX2", "NEON", or "Scalar")
 */
const char* GetImplementationName();

}  // namespace nvecd::vectors::simd
