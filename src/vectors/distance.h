/**
 * @file distance.h
 * @brief Vector distance and similarity calculation functions
 *
 * Provides optimized implementations of common distance metrics:
 * - Dot Product (inner product)
 * - Cosine Similarity (normalized dot product)
 * - L2 Distance (Euclidean distance)
 *
 * All functions automatically use the best available SIMD implementation
 * (AVX2, NEON, or scalar fallback) via runtime CPU detection.
 */

#pragma once

#include <cmath>
#include <vector>

#include "vectors/distance_simd.h"

namespace nvecd::vectors {

/**
 * @brief Calculate dot product between two vectors
 *
 * Dot product: sum(lhs[i] * rhs[i])
 * Higher values indicate greater similarity.
 * Uses SIMD-optimized implementation when available.
 *
 * @param lhs First vector (left-hand side)
 * @param rhs Second vector (right-hand side)
 * @return Dot product value, or 0.0 if dimensions mismatch
 */
inline float DotProduct(const std::vector<float>& lhs, const std::vector<float>& rhs) {
  if (lhs.size() != rhs.size() || lhs.empty()) {
    return 0.0F;
  }

  return simd::GetOptimalImpl().dot_product(lhs.data(), rhs.data(), lhs.size());
}

/**
 * @brief Calculate L2 norm (magnitude) of a vector
 *
 * L2 norm: sqrt(sum(vec[i]^2))
 * Uses SIMD-optimized implementation when available.
 *
 * @param vec Vector
 * @return L2 norm value
 */
inline float L2Norm(const std::vector<float>& vec) {
  if (vec.empty()) {
    return 0.0F;
  }

  return simd::GetOptimalImpl().l2_norm(vec.data(), vec.size());
}

/**
 * @brief Calculate cosine similarity between two vectors
 *
 * Cosine similarity: dot(lhs, rhs) / (||lhs|| * ||rhs||)
 * Returns value in [-1, 1], where 1 means identical direction.
 * Uses SIMD-optimized implementation when available.
 *
 * @param lhs First vector (left-hand side)
 * @param rhs Second vector (right-hand side)
 * @return Cosine similarity, or 0.0 if dimensions mismatch or either vector is zero
 */
inline float CosineSimilarity(const std::vector<float>& lhs, const std::vector<float>& rhs) {
  if (lhs.size() != rhs.size() || lhs.empty()) {
    return 0.0F;
  }

  const auto& impl = simd::GetOptimalImpl();
  float dot = impl.dot_product(lhs.data(), rhs.data(), lhs.size());
  float norm_lhs = impl.l2_norm(lhs.data(), lhs.size());
  float norm_rhs = impl.l2_norm(rhs.data(), rhs.size());

  constexpr float kNormEpsilon = 1e-7F;
  if (norm_lhs < kNormEpsilon || norm_rhs < kNormEpsilon) {
    return 0.0F;  // Undefined for zero/near-zero vectors
  }

  return dot / (norm_lhs * norm_rhs);
}

/**
 * @brief Cosine similarity with pre-computed L2 norms
 *
 * Avoids redundant norm computation for stored vectors whose norms
 * have been pre-computed in compact storage.
 *
 * @param lhs Pointer to first vector data
 * @param rhs Pointer to second vector data
 * @param dim Vector dimension
 * @param norm_lhs Pre-computed L2 norm of lhs
 * @param norm_rhs Pre-computed L2 norm of rhs
 * @return Cosine similarity, or 0.0 if either norm is near-zero
 */
inline float CosineSimilarityPreNorm(const float* lhs, const float* rhs, size_t dim, float norm_lhs, float norm_rhs) {
  constexpr float kNormEpsilon = 1e-7F;
  if (norm_lhs < kNormEpsilon || norm_rhs < kNormEpsilon) {
    return 0.0F;
  }
  float dot = simd::GetOptimalImpl().dot_product(lhs, rhs, dim);
  return dot / (norm_lhs * norm_rhs);
}

/**
 * @brief Calculate L2 (Euclidean) distance between two vectors
 *
 * L2 distance: sqrt(sum((lhs[i] - rhs[i])^2))
 * Lower values indicate greater similarity.
 * Uses SIMD-optimized implementation when available.
 *
 * @param lhs First vector (left-hand side)
 * @param rhs Second vector (right-hand side)
 * @return L2 distance, or 0.0 if dimensions mismatch
 */
inline float L2Distance(const std::vector<float>& lhs, const std::vector<float>& rhs) {
  if (lhs.size() != rhs.size() || lhs.empty()) {
    return 0.0F;
  }

  return simd::GetOptimalImpl().l2_distance(lhs.data(), rhs.data(), lhs.size());
}

/**
 * @brief Normalize a vector to unit length (L2 norm = 1)
 *
 * Modifies the input vector in-place.
 *
 * @param vec Vector to normalize
 * @return True if normalized, false if vector is zero
 */
inline bool Normalize(std::vector<float>& vec) {
  float norm = L2Norm(vec);
  constexpr float kNormEpsilon = 1e-7F;
  if (norm < kNormEpsilon) {
    return false;  // Cannot normalize zero/near-zero vector
  }

  for (float& val : vec) {
    val /= norm;
  }
  return true;
}

/**
 * @brief Create a normalized copy of a vector
 *
 * @param vec Input vector
 * @return Normalized copy, or empty vector if input is zero
 */
inline std::vector<float> NormalizedCopy(const std::vector<float>& vec) {
  std::vector<float> result = vec;
  if (!Normalize(result)) {
    return {};  // Return empty for zero vector
  }
  return result;
}

}  // namespace nvecd::vectors
