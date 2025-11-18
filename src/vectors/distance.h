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
 * Dot product: sum(lhs[i] * rhs[i])
 * Higher values indicate greater similarity.
 *
 * @param lhs First vector (left-hand side)
 * @param rhs Second vector (right-hand side)
 * @return Dot product value, or 0.0 if dimensions mismatch
 */
inline float DotProduct(const std::vector<float>& lhs, const std::vector<float>& rhs) {
  if (lhs.size() != rhs.size() || lhs.empty()) {
    return 0.0F;
  }

  float sum = 0.0F;
  for (size_t i = 0; i < lhs.size(); ++i) {
    sum += lhs[i] * rhs[i];
  }
  return sum;
}

/**
 * @brief Calculate L2 norm (magnitude) of a vector
 *
 * L2 norm: sqrt(sum(vec[i]^2))
 *
 * @param vec Vector
 * @return L2 norm value
 */
inline float L2Norm(const std::vector<float>& vec) {
  if (vec.empty()) {
    return 0.0F;
  }

  float sum_sq = 0.0F;
  for (float val : vec) {
    sum_sq += val * val;
  }
  return std::sqrt(sum_sq);
}

/**
 * @brief Calculate cosine similarity between two vectors
 *
 * Cosine similarity: dot(lhs, rhs) / (||lhs|| * ||rhs||)
 * Returns value in [-1, 1], where 1 means identical direction.
 *
 * @param lhs First vector (left-hand side)
 * @param rhs Second vector (right-hand side)
 * @return Cosine similarity, or 0.0 if dimensions mismatch or either vector is zero
 */
inline float CosineSimilarity(const std::vector<float>& lhs, const std::vector<float>& rhs) {
  if (lhs.size() != rhs.size() || lhs.empty()) {
    return 0.0F;
  }

  float dot = DotProduct(lhs, rhs);
  float norm_lhs = L2Norm(lhs);
  float norm_rhs = L2Norm(rhs);

  if (norm_lhs == 0.0F || norm_rhs == 0.0F) {
    return 0.0F;  // Undefined for zero vectors
  }

  return dot / (norm_lhs * norm_rhs);
}

/**
 * @brief Calculate L2 (Euclidean) distance between two vectors
 *
 * L2 distance: sqrt(sum((lhs[i] - rhs[i])^2))
 * Lower values indicate greater similarity.
 *
 * @param lhs First vector (left-hand side)
 * @param rhs Second vector (right-hand side)
 * @return L2 distance, or 0.0 if dimensions mismatch
 */
inline float L2Distance(const std::vector<float>& lhs, const std::vector<float>& rhs) {
  if (lhs.size() != rhs.size() || lhs.empty()) {
    return 0.0F;
  }

  float sum_sq = 0.0F;
  for (size_t i = 0; i < lhs.size(); ++i) {
    float diff = lhs[i] - rhs[i];
    sum_sq += diff * diff;
  }
  return std::sqrt(sum_sq);
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
  if (norm == 0.0F) {
    return false;  // Cannot normalize zero vector
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
