/**
 * @file distance_simd.cpp
 * @brief Selects the distance kernels for the CPU the server is running on
 *
 * This translation unit exists so the selection happens exactly once. It is
 * compiled for the baseline ISA: the AVX2 kernels live in
 * distance_avx2_kernels.cpp, the only unit built with -mavx2, so a portable
 * build executes nothing above the baseline before the CPU check below.
 *
 * Compile-time availability and runtime capability stay separate:
 * NVECD_HAVE_AVX2_KERNELS decides whether the kernels were built at all, and
 * the CpuInfo check decides whether this machine may execute them.
 */

#include "vectors/distance_simd.h"

#include "vectors/cpu_features.h"
#include "vectors/distance_scalar.h"

#ifdef __ARM_NEON
#include "vectors/distance_neon.h"
#endif

namespace nvecd::vectors::simd {

#ifdef NVECD_HAVE_AVX2_KERNELS
extern const DistanceFunctions kAvx2Functions;
#endif

const DistanceFunctions& GetOptimalImpl() {
  // Static initialization is thread-safe in C++11+
  static const DistanceFunctions impl = []() {
    [[maybe_unused]] CpuInfo cpu = DetectCpuFeatures();

#ifdef NVECD_HAVE_AVX2_KERNELS
    // AVX2 kernels built, check runtime support
    if (cpu.has_avx2) {
      return kAvx2Functions;
    }
#endif

#ifdef __ARM_NEON
    // NEON available at compile-time, check runtime support
    if (cpu.has_neon) {
      return DistanceFunctions{DotProductNEON, L2NormNEON, L2DistanceNEON, "NEON"};
    }
#endif

    // Fallback to scalar implementation
    return DistanceFunctions{DotProductScalar, L2NormScalar, L2DistanceScalar, "Scalar"};
  }();

  return impl;
}

const char* GetImplementationName() {
  return GetOptimalImpl().implementation_name;
}

}  // namespace nvecd::vectors::simd
