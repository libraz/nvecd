/**
 * @file cpu_features.h
 * @brief CPU feature detection for SIMD optimization
 *
 * Provides runtime detection of CPU SIMD capabilities (AVX2, NEON)
 * for optimal vector operation dispatch.
 */

#pragma once

#include <cstdint>

namespace nvecd::vectors::simd {

/**
 * @brief CPU SIMD feature flags
 */
enum class CpuFeatures : std::uint8_t {
  kScalar = 0,     ///< No SIMD support (fallback)
  kSSE2 = 1 << 0,  ///< x86 SSE2 (baseline, not used yet)
  kAVX2 = 1 << 1,  ///< x86_64 AVX2 (256-bit SIMD)
  kNEON = 1 << 2,  ///< ARM NEON (128-bit SIMD)
};

/**
 * @brief CPU information and capabilities
 */
struct CpuInfo {
  const char* arch_name;  ///< Architecture name (e.g., "x86_64", "ARM64")
  bool has_avx2;          ///< True if AVX2 is available
  bool has_neon;          ///< True if NEON is available
};

/**
 * @brief Detect CPU features at runtime
 *
 * This function performs runtime detection of CPU capabilities.
 * It is thread-safe and typically called once at startup.
 *
 * Detection strategy:
 * - x86_64: Uses __builtin_cpu_supports() for AVX2
 * - ARM64: Assumes NEON (baseline for AArch64)
 * - ARM32: Checks compile-time __ARM_NEON flag
 *
 * @return CPU information structure
 */
CpuInfo DetectCpuFeatures();

}  // namespace nvecd::vectors::simd
