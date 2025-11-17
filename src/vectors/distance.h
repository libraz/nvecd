/**
 * @file distance.h
 * @brief Vector distance and similarity calculation functions
 *
 * Provides optimized implementations of common distance metrics:
 * - Dot Product (inner product)
 * - Cosine Similarity (normalized dot product)
 * - L2 Distance (Euclidean distance)
 */

#pragma once

#include <cmath>
#include <vector>

namespace nvecd::vectors {

/**
 * @brief Calculate dot product between two vectors
 *
 * Dot product: sum(a[i] * b[i])
 * Higher values indicate greater similarity.
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

  float sum = 0.0f;
  for (size_t i = 0; i < a.size(); ++i) {
    sum += a[i] * b[i];
  }
  return sum;
}

/**
 * @brief Calculate L2 norm (magnitude) of a vector
 *
 * L2 norm: sqrt(sum(v[i]^2))
 *
 * @param v Vector
 * @return L2 norm value
 */
inline float L2Norm(const std::vector<float>& v) {
  if (v.empty()) {
    return 0.0f;
  }

  float sum_sq = 0.0f;
  for (float val : v) {
    sum_sq += val * val;
  }
  return std::sqrt(sum_sq);
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
 * @param a First vector
 * @param b Second vector
 * @return L2 distance, or 0.0 if dimensions mismatch
 */
inline float L2Distance(const std::vector<float>& a,
                        const std::vector<float>& b) {
  if (a.size() != b.size() || a.empty()) {
    return 0.0f;
  }

  float sum_sq = 0.0f;
  for (size_t i = 0; i < a.size(); ++i) {
    float diff = a[i] - b[i];
    sum_sq += diff * diff;
  }
  return std::sqrt(sum_sq);
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
