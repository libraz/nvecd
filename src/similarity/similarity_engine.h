/**
 * @file similarity_engine.h
 * @brief Similarity search engine with multiple search modes
 *
 * Provides unified interface for:
 * - Events-based similarity (co-occurrence patterns)
 * - Vectors-based similarity (vector distance)
 * - Fusion similarity (weighted combination)
 * - Vector query similarity (SIMV)
 */

#pragma once

#include <string>
#include <vector>

#include "config/config.h"
#include "events/co_occurrence_index.h"
#include "events/event_store.h"
#include "utils/error.h"
#include "utils/expected.h"
#include "vectors/vector_store.h"

namespace nvecd::similarity {

/**
 * @brief Similarity search result
 */
struct SimilarityResult {
  std::string id;  ///< Item ID
  float score;     ///< Similarity score (higher = more similar)

  SimilarityResult() = default;
  SimilarityResult(std::string id_, float score_)
      : id(std::move(id_)), score(score_) {}

  /**
   * @brief Compare for sorting (descending by score)
   */
  bool operator<(const SimilarityResult& other) const {
    return score > other.score;  // Descending order
  }
};

/**
 * @brief Similarity search engine
 *
 * Coordinates between EventStore, CoOccurrenceIndex, and VectorStore
 * to provide multiple similarity search modes.
 *
 * Search modes:
 * - Events: Based on co-occurrence patterns
 * - Vectors: Based on vector distance (dot product, cosine, L2)
 * - Fusion: Weighted combination of events and vectors
 * - Vector query (SIMV): Search by arbitrary vector
 *
 * Thread-safety:
 * - All methods are thread-safe (delegates to thread-safe components)
 *
 * Example:
 * @code
 * SimilarityEngine engine(&event_store, &co_index, &vector_store, config);
 * auto results = engine.SearchByIdFusion("item123", 10);
 * if (results) {
 *   for (const auto& result : *results) {
 *     std::cout << result.id << ": " << result.score << std::endl;
 *   }
 * }
 * @endcode
 */
class SimilarityEngine {
 public:
  /**
   * @brief Construct similarity engine
   *
   * @param event_store Event store (not owned, must outlive this object)
   * @param co_index Co-occurrence index (not owned, must outlive this object)
   * @param vector_store Vector store (not owned, must outlive this object)
   * @param config Similarity configuration
   */
  SimilarityEngine(events::EventStore* event_store,
                   events::CoOccurrenceIndex* co_index,
                   vectors::VectorStore* vector_store,
                   const config::SimilarityConfig& config);

  /**
   * @brief Search similar items using events (co-occurrence)
   *
   * Uses co-occurrence index to find items that frequently appear
   * in the same contexts as the query item.
   *
   * @param id Query item ID
   * @param top_k Maximum number of results
   * @return Expected<vector<SimilarityResult>, Error> Results or error
   */
  utils::Expected<std::vector<SimilarityResult>, utils::Error>
  SearchByIdEvents(const std::string& id, int top_k);

  /**
   * @brief Search similar items using vectors (distance)
   *
   * Uses vector store to find items with similar vector representations.
   * Distance metric is configured in VectorsConfig.
   *
   * @param id Query item ID
   * @param top_k Maximum number of results
   * @return Expected<vector<SimilarityResult>, Error> Results or error
   */
  utils::Expected<std::vector<SimilarityResult>, utils::Error>
  SearchByIdVectors(const std::string& id, int top_k);

  /**
   * @brief Search similar items using fusion (events + vectors)
   *
   * Combines events-based and vectors-based scores using weighted sum:
   * score = alpha * vector_score + beta * event_score
   *
   * @param id Query item ID
   * @param top_k Maximum number of results
   * @return Expected<vector<SimilarityResult>, Error> Results or error
   */
  utils::Expected<std::vector<SimilarityResult>, utils::Error>
  SearchByIdFusion(const std::string& id, int top_k);

  /**
   * @brief Search similar items using vector query (SIMV)
   *
   * Finds items with vectors similar to the provided query vector.
   * Does not require the query vector to be stored.
   *
   * @param query_vector Query vector
   * @param top_k Maximum number of results
   * @return Expected<vector<SimilarityResult>, Error> Results or error
   */
  utils::Expected<std::vector<SimilarityResult>, utils::Error>
  SearchByVector(const std::vector<float>& query_vector, int top_k);

 private:
  /**
   * @brief Validate top_k parameter
   * @param top_k Requested number of results
   * @return Expected<int, Error> Validated top_k or error
   */
  utils::Expected<int, utils::Error> ValidateTopK(int top_k) const;

  /**
   * @brief Normalize scores to [0, 1] range
   *
   * Uses min-max normalization if there are multiple results.
   *
   * @param results Results to normalize (modified in-place)
   */
  void NormalizeScores(std::vector<SimilarityResult>& results) const;

  /**
   * @brief Merge and sort results from multiple sources
   *
   * @param results Results to merge and sort
   * @param top_k Maximum number of results to return
   * @return Top-k results sorted by score descending
   */
  std::vector<SimilarityResult> MergeAndSelectTopK(
      std::vector<SimilarityResult> results, int top_k) const;

  [[maybe_unused]] events::EventStore* event_store_;             ///< Event store (not owned, reserved for future use)
  events::CoOccurrenceIndex* co_index_;         ///< Co-occurrence index (not owned)
  vectors::VectorStore* vector_store_;          ///< Vector store (not owned)
  config::SimilarityConfig config_;             ///< Configuration
};

}  // namespace nvecd::similarity
