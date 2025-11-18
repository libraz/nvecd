/**
 * @file cpu_features.cpp
 * @brief CPU feature detection implementation
 */

#include "vectors/cpu_features.h"

namespace nvecd::vectors::simd {

CpuInfo DetectCpuFeatures() {
  CpuInfo info{};
  info.arch_name = "Unknown";
  info.has_avx2 = false;
  info.has_neon = false;

#if defined(__x86_64__) || defined(_M_X64)
  // x86_64 architecture
  info.arch_name = "x86_64";

#ifdef __AVX2__
  // AVX2 compiled in - check runtime support
#if defined(__GNUC__) || defined(__clang__)
  // GCC/Clang: Use builtin runtime detection
  if (__builtin_cpu_supports("avx2")) {
    info.has_avx2 = true;
  }
#else
  // MSVC or other compilers: Assume AVX2 if compiled with /arch:AVX2
  info.has_avx2 = true;
#endif
#endif  // __AVX2__

#elif defined(__aarch64__) || defined(_M_ARM64)
  // ARM64 (AArch64) architecture
  info.arch_name = "ARM64";

#ifdef __ARM_NEON
  // NEON is baseline for AArch64
  info.has_neon = true;
#endif

#elif defined(__arm__) || defined(_M_ARM)
  // ARM32 (ARMv7) architecture
  info.arch_name = "ARM32";

#ifdef __ARM_NEON
  // NEON available on ARMv7 with NEON extension
  info.has_neon = true;
#endif

#else
  // Unknown or unsupported architecture
  info.arch_name = "Unknown";
#endif

  return info;
}

}  // namespace nvecd::vectors::simd
