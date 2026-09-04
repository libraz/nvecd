/**
 * @file distance_simd.cpp
 * @brief Selects the distance kernels for the CPU the server is running on
 *
 * This translation unit exists so the selection happens exactly once, in a
 * target that is compiled with the ISA flags. `-mavx2` is PRIVATE to
 * nvecd_vectors, so `__AVX2__` is defined only while compiling this library.
 * Were the selection an inline function in the header, every other target would
 * compile a body with the AVX2 branch preprocessed away, leaving several
 * definitions of one inline function and letting the linker decide which the
 * server actually runs.
 *
 * Compile-time availability and runtime capability stay separate: the #ifdef
 * decides whether a kernel was built at all, and the CpuInfo check decides
 * whether this machine may execute it.
 */

#include "vectors/distance_simd.h"

#include "vectors/cpu_features.h"
#include "vectors/distance_scalar.h"

#ifdef __AVX2__
#include "vectors/distance_avx2.h"
#endif

#ifdef __ARM_NEON
#include "vectors/distance_neon.h"
#endif

namespace nvecd::vectors::simd {

const DistanceFunctions& GetOptimalImpl() {
  // Static initialization is thread-safe in C++11+
  static const DistanceFunctions impl = []() {
    [[maybe_unused]] CpuInfo cpu = DetectCpuFeatures();

#ifdef __AVX2__
    // AVX2 available at compile-time, check runtime support
    if (cpu.has_avx2) {
      return DistanceFunctions{DotProductAVX2, L2NormAVX2, L2DistanceAVX2, "AVX2"};
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
