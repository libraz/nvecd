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

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include "config/config.h"
#include "events/co_occurrence_index.h"
#include "events/event_store.h"
#include "similarity/similarity_result.h"
#include "utils/error.h"
#include "utils/expected.h"
#include "vectors/distance.h"
#include "vectors/ivf_index.h"
#include "vectors/vector_store.h"

namespace nvecd::similarity {

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
 *     std::cout << result.item_id << ": " << result.score << std::endl;
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
  SimilarityEngine(events::EventStore* event_store, events::CoOccurrenceIndex* co_index,
                   vectors::VectorStore* vector_store, const config::SimilarityConfig& config,
                   const config::VectorsConfig& vectors_config = config::VectorsConfig{});

  /// @brief Destructor joins background training thread if running
  ~SimilarityEngine();

  // Non-copyable, non-movable (owns a thread)
  SimilarityEngine(const SimilarityEngine&) = delete;
  SimilarityEngine& operator=(const SimilarityEngine&) = delete;
  SimilarityEngine(SimilarityEngine&&) = delete;
  SimilarityEngine& operator=(SimilarityEngine&&) = delete;

  /**
   * @brief Search similar items using events (co-occurrence)
   *
   * Uses co-occurrence index to find items that frequently appear
   * in the same contexts as the query item.
   *
   * @param item_id Query item ID
   * @param top_k Maximum number of results
   * @return Expected<vector<SimilarityResult>, Error> Results or error
   */
  utils::Expected<std::vector<SimilarityResult>, utils::Error> SearchByIdEvents(const std::string& item_id, int top_k);

  /**
   * @brief Search similar items using vectors (distance)
   *
   * Uses vector store to find items with similar vector representations.
   * Distance metric is configured in VectorsConfig.
   *
   * @param item_id Query item ID
   * @param top_k Maximum number of results
   * @return Expected<vector<SimilarityResult>, Error> Results or error
   */
  utils::Expected<std::vector<SimilarityResult>, utils::Error> SearchByIdVectors(const std::string& item_id, int top_k);

  /**
   * @brief Search similar items using fusion (events + vectors)
   *
   * Combines events-based and vectors-based scores using weighted sum:
   * score = alpha * vector_score + beta * event_score
   *
   * @param item_id Query item ID
   * @param top_k Maximum number of results
   * @return Expected<vector<SimilarityResult>, Error> Results or error
   */
  utils::Expected<std::vector<SimilarityResult>, utils::Error> SearchByIdFusion(
      const std::string& item_id, int top_k, std::optional<bool> adaptive = std::nullopt);

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
  utils::Expected<std::vector<SimilarityResult>, utils::Error> SearchByVector(const std::vector<float>& query_vector,
                                                                              int top_k);

  /**
   * @brief Notify the engine that a vector was added or updated
   *
   * If IVF is enabled and trained, adds the vector to the IVF index.
   * If IVF is enabled but not yet trained, checks if the training
   * threshold has been reached and triggers training if so.
   *
   * @param compact_index Index in compact storage
   * @param vector Pointer to vector data
   */
  void NotifyVectorAdded(size_t compact_index, const float* vector);

  /**
   * @brief Notify the engine that a vector was removed
   * @param compact_index Index that was removed
   */
  void NotifyVectorRemoved(size_t compact_index);

  /**
   * @brief Check if IVF index is trained
   * @return True if IVF is enabled and trained
   */
  bool IsIvfTrained() const;

  /**
   * @brief Force IVF index training/retraining now
   *
   * Triggers asynchronous training using current vector data.
   * Does nothing if IVF is disabled or training is already in progress.
   */
  void ForceIvfTrain();

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
  static void NormalizeScores(std::vector<SimilarityResult>& results);

  /**
   * @brief Merge and sort results from multiple sources
   *
   * @param results Results to merge and sort
   * @param top_k Maximum number of results to return
   * @return Top-k results sorted by score descending
   */
  static std::vector<SimilarityResult> MergeAndSelectTopK(std::vector<SimilarityResult> results, int top_k);

  /// @brief Distance function type for similarity computation
  using DistanceFunc = std::function<float(const std::vector<float>&, const std::vector<float>&)>;

  /**
   * @brief Select distance function based on metric name
   * @param metric Distance metric name ("cosine", "dot", "l2")
   * @return Distance function
   */
  static DistanceFunc SelectDistanceFunction(const std::string& metric);

  /// @brief Generate random sample indices using reservoir sampling
  std::vector<size_t> SampleIndices(size_t total, size_t sample_size) const;

  /// @brief Search vectors only among candidate IDs (pre-filtered)
  utils::Expected<std::vector<SimilarityResult>, utils::Error> SearchByIdVectorsFiltered(
      const std::string& item_id, const std::unordered_set<std::string>& candidate_ids, int top_k);

  /**
   * @brief Compute adaptive fusion weights based on item maturity
   * @param neighbor_count Number of co-occurrence neighbors
   * @return Pair of (alpha, beta) weights
   */
  std::pair<float, float> ComputeAdaptiveWeights(size_t neighbor_count) const;

  [[maybe_unused]] events::EventStore* event_store_;  ///< Event store (not owned)
  events::CoOccurrenceIndex* co_index_;               ///< Co-occurrence index (not owned)
  vectors::VectorStore* vector_store_;                ///< Vector store (not owned)
  config::SimilarityConfig config_;                   ///< Configuration
  DistanceFunc distance_func_;                        ///< Distance function for similarity
  bool use_prenorm_ = false;                          ///< Whether to use pre-computed norm optimization (cosine only)

  /// IVF index for approximate nearest neighbor search (null if disabled)
  std::unique_ptr<vectors::IvfIndex> ivf_index_;

  /// Background thread for asynchronous IVF training
  std::unique_ptr<std::thread> ivf_train_thread_;

  /// True while IVF training is in progress (search falls back to brute-force)
  std::atomic<bool> ivf_training_{false};

  /**
   * @brief Attempt to train the IVF index if threshold is met
   *
   * Launches training asynchronously in a background thread.
   * While training is in progress, search falls back to brute-force.
   * Called internally after vector additions.
   */
  void MaybeTrainIvfIndex();

  /**
   * @brief Join the background training thread if it is joinable
   */
  void JoinTrainThread();
};

}  // namespace nvecd::similarity
