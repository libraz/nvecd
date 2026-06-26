/**
 * @file similarity_result.h
 * @brief Similarity search result type
 *
 * Separated from similarity_engine.h to avoid circular dependencies
 * (cache module needs SimilarityResult but not the full engine).
 */

#pragma once

#include <string>

namespace nvecd::similarity {

/**
 * @brief Similarity search result
 */
struct SimilarityResult {
  std::string item_id;  ///< Item ID
  float score{0.0F};    ///< Similarity score (higher = more similar)

  SimilarityResult() = default;
  SimilarityResult(std::string item_id_, float score_) : item_id(std::move(item_id_)), score(score_) {}

  /**
   * @brief Compare for sorting (descending by score)
   *
   * Ties on score are broken by item_id (ascending) so that ordering is
   * deterministic across runs and nodes regardless of insertion order.
   */
  bool operator<(const SimilarityResult& other) const {
    if (score != other.score) {
      return score > other.score;  // Primary: descending score
    }
    return item_id < other.item_id;  // Secondary: ascending id for stable order
  }
};

}  // namespace nvecd::similarity
