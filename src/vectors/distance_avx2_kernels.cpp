/**
 * @file distance_avx2_kernels.cpp
 * @brief Exports the AVX2 distance kernels as a dispatch table
 *
 * This is the only translation unit of the library compiled with -mavx2, so
 * no AVX2 instruction can be emitted outside the kernels themselves. The table
 * is constant-initialized: reading it executes no vector code, which keeps the
 * runtime CPU check in distance_simd.cpp ahead of the first AVX2 instruction.
 */

#include "vectors/distance_avx2.h"
#include "vectors/distance_simd.h"

namespace nvecd::vectors::simd {

extern const DistanceFunctions kAvx2Functions;
const DistanceFunctions kAvx2Functions{DotProductAVX2, L2NormAVX2, L2DistanceAVX2, "AVX2"};

}  // namespace nvecd::vectors::simd
