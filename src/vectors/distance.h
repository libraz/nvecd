/**
 * @file distance.h
 * @brief Vector distance and similarity calculation functions
 *
 * Provides optimized implementations of common distance metrics:
 * - Dot Product (inner product)
 * - Cosine Similarity (normalized dot product)
 * - L2 Distance (Euclidean distance)
 *
 * Implementation uses SIMD acceleration (AVX2/NEON) when available,
 * with automatic runtime detection and fallback to scalar code.
 */

#pragma once

#include <cmath>
#include <vector>

#include "vectors/distance_simd.h"

namespace nvecd::vectors {

/**
 * @brief Calculate dot product between two vectors
 *
 * Dot product: sum(a[i] * b[i])
 * Higher values indicate greater similarity.
 *
 * Uses SIMD optimization (AVX2/NEON) when available.
 *
 * @param a First vector
 * @param b Second vector
 * @return Dot product value, or 0.0 if dimensions mismatch
 */
inline float DotProduct(const std::vector<float>& a,
                        const std::vector<float>& b) {
  if (a.size() != b.size() || a.empty()) {
    return 0.0f;
  }

  // Dispatch to optimal SIMD implementation
  const auto& impl = simd::GetOptimalImpl();
  return impl.dot_product(a.data(), b.data(), a.size());
}

/**
 * @brief Calculate L2 norm (magnitude) of a vector
 *
 * L2 norm: sqrt(sum(v[i]^2))
 *
 * Uses SIMD optimization (AVX2/NEON) when available.
 *
 * @param v Vector
 * @return L2 norm value
 */
inline float L2Norm(const std::vector<float>& v) {
  if (v.empty()) {
    return 0.0f;
  }

  // Dispatch to optimal SIMD implementation
  const auto& impl = simd::GetOptimalImpl();
  return impl.l2_norm(v.data(), v.size());
}

/**
 * @brief Calculate cosine similarity between two vectors
 *
 * Cosine similarity: dot(a, b) / (||a|| * ||b||)
 * Returns value in [-1, 1], where 1 means identical direction.
 *
 * @param a First vector
 * @param b Second vector
 * @return Cosine similarity, or 0.0 if dimensions mismatch or either vector is zero
 */
inline float CosineSimilarity(const std::vector<float>& a,
                              const std::vector<float>& b) {
  if (a.size() != b.size() || a.empty()) {
    return 0.0f;
  }

  float dot = DotProduct(a, b);
  float norm_a = L2Norm(a);
  float norm_b = L2Norm(b);

  if (norm_a == 0.0f || norm_b == 0.0f) {
    return 0.0f;  // Undefined for zero vectors
  }

  return dot / (norm_a * norm_b);
}

/**
 * @brief Calculate L2 (Euclidean) distance between two vectors
 *
 * L2 distance: sqrt(sum((a[i] - b[i])^2))
 * Lower values indicate greater similarity.
 *
 * Uses SIMD optimization (AVX2/NEON) when available.
 *
 * @param a First vector
 * @param b Second vector
 * @return L2 distance, or 0.0 if dimensions mismatch
 */
inline float L2Distance(const std::vector<float>& a,
                        const std::vector<float>& b) {
  if (a.size() != b.size() || a.empty()) {
    return 0.0f;
  }

  // Dispatch to optimal SIMD implementation
  const auto& impl = simd::GetOptimalImpl();
  return impl.l2_distance(a.data(), b.data(), a.size());
}

/**
 * @brief Normalize a vector to unit length (L2 norm = 1)
 *
 * Modifies the input vector in-place.
 *
 * @param v Vector to normalize
 * @return True if normalized, false if vector is zero
 */
inline bool Normalize(std::vector<float>& v) {
  float norm = L2Norm(v);
  if (norm == 0.0f) {
    return false;  // Cannot normalize zero vector
  }

  for (float& val : v) {
    val /= norm;
  }
  return true;
}

/**
 * @brief Create a normalized copy of a vector
 *
 * @param v Input vector
 * @return Normalized copy, or empty vector if input is zero
 */
inline std::vector<float> NormalizedCopy(const std::vector<float>& v) {
  std::vector<float> result = v;
  if (!Normalize(result)) {
    return {};  // Return empty for zero vector
  }
  return result;
}

}  // namespace nvecd::vectors
