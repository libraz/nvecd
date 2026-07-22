/**
 * @file co_occurrence_index.h
 * @brief Co-occurrence scoring index for event-based similarity
 *
 * Tracks which items co-occur in event contexts and provides similarity
 * scores based on co-occurrence patterns.
 */

#pragma once

#include <atomic>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "events/event_store.h"

namespace nvecd::events {

/**
 * @brief Co-occurrence index statistics
 */
struct CoOccurrenceIndexStatistics {
  size_t tracked_ids = 0;   ///< Number of tracked item IDs
  size_t co_pairs = 0;      ///< Number of co-occurrence pairs
  size_t memory_bytes = 0;  ///< Estimated memory usage in bytes
};

/**
 * @brief Co-occurrence index for event-based similarity
 *
 * Maintains a symmetric matrix of co-occurrence scores between items.
 * When events occur in the same context, their co-occurrence scores increase.
 *
 * Algorithm:
 * - For each context, compute pairwise co-occurrence scores
 * - Score = sum(event1.score * event2.score) for all event pairs
 * - Supports periodic decay to favor recent co-occurrences
 *
 * Thread-safety:
 * - Multiple concurrent readers (GetSimilar, GetScore)
 * - Exclusive writer (UpdateFromEvents, ApplyDecay)
 *
 * Example:
 * @code
 * CoOccurrenceIndex index;
 * std::vector<Event> events = {
 *   {"item1", 10, timestamp},
 *   {"item2", 20, timestamp},
 *   {"item3", 15, timestamp}
 * };
 * index.UpdateFromEvents("user123", events);
 * auto similar = index.GetSimilar("item1", 10);
 * @endcode
 */
class CoOccurrenceIndex {
 public:
  /**
   * @brief Configuration for pruning behavior
   */
  struct Config {
    uint32_t max_neighbors_per_item = 0;    ///< Max neighbors per item (0 = unlimited)
    float min_support = 0.0F;               ///< Min score threshold (0 = no pruning)
    uint32_t negative_max_propagation = 1;  ///< Max hops for negative signal (0 = disabled)
  };

  /**
   * @brief Construct empty co-occurrence index with default config
   */
  CoOccurrenceIndex() = default;

  /**
   * @brief Construct co-occurrence index with pruning config
   * @param config Pruning configuration
   */
  explicit CoOccurrenceIndex(const Config& config);

  /**
   * @brief Update co-occurrence scores from events
   *
   * Computes pairwise co-occurrence scores for all events in the context
   * and adds them to the existing scores.
   *
   * @param ctx Context identifier (used for logging, not stored)
   * @param events Events from a single context
   */
  void UpdateFromEvents(const std::string& ctx, const std::vector<Event>& events);

  /**
   * @brief Update co-occurrence with temporal decay
   * @param ctx Context identifier
   * @param events Events from context
   * @param temporal_enabled Enable temporal weighting
   * @param half_life_sec Half-life in seconds for decay
   */
  void UpdateFromEvents(const std::string& ctx, const std::vector<Event>& events, bool temporal_enabled,
                        double half_life_sec);

  /**
   * @brief Update co-occurrence under existing write lock
   * @note Caller must hold write lock via AcquireWriteLock()
   */
  void UpdateFromEventsLocked(const std::string& ctx, const std::vector<Event>& events, bool temporal_enabled,
                              double half_life_sec);

  /**
   * @brief Incrementally add co-occurrence pairs for a single new event
   *
   * Adds only the pairs (new_event, p) for each p in @p prior_events, each
   * counted exactly once (both directions, symmetric). This is the correct
   * incremental update for streaming ingestion: when a new event arrives in a
   * context whose buffer already contains @p prior_events, only the new pairs
   * involving @p new_event need to be scored. It avoids the O(N^2) re-scan
   * (and resulting over-count) of re-processing the entire buffer per event.
   *
   * Self-pairs (same item_id) are skipped. Temporal decay, when enabled, is
   * computed relative to the newer event in each pair, matching
   * UpdateFromEvents regardless of arrival order. The pruning hook
   * (max_neighbors / min_support) is applied to the affected item, matching
   * UpdateFromEvents.
   *
   * @param ctx Context identifier (used for logging, not stored)
   * @param prior_events Events already present in the context before new_event
   * @param new_event The newly arrived event
   * @param temporal_enabled Enable temporal weighting
   * @param half_life_sec Half-life in seconds for decay
   */
  void AddEventIncremental(const std::string& ctx, const std::vector<Event>& prior_events, const Event& new_event,
                           bool temporal_enabled, double half_life_sec);

  /**
   * @brief Incrementally add co-occurrence pairs under an existing write lock
   * @note Caller must hold write lock via AcquireWriteLock()
   * @see AddEventIncremental
   */
  void AddEventIncrementalLocked(const std::string& ctx, const std::vector<Event>& prior_events, const Event& new_event,
                                 bool temporal_enabled, double half_life_sec);

  /**
   * @brief Apply negative signal for a removed item under existing write lock
   * @note Caller must hold write lock via AcquireWriteLock()
   */
  void ApplyNegativeSignalLocked(const std::string& removed_id, const std::vector<Event>& context_events,
                                 double negative_weight);

  /**
   * @brief Options controlling how an ingested event updates the index
   */
  struct IngestOptions {
    bool temporal_enabled = false;  ///< Enable temporal decay weighting
    double half_life_sec = 0.0;     ///< Half-life in seconds for temporal decay
    bool negative_signals = false;  ///< Apply negative signal on DEL events
    double negative_weight = 0.0;   ///< Weight for negative signal reduction
  };

  /**
   * @brief Apply the incremental co-occurrence update for one ingested event
   *
   * Shared ingestion routine used by both the TCP and HTTP EVENT handlers so
   * the incremental, atomic update logic lives in a single place. Given the
   * prior buffer state and the newly stored event (as returned by
   * EventStore::AddEventAndGetPrior), this adds only the new pairs once and,
   * for DEL events with negative signals enabled, applies the negative signal
   * against the prior buffer. The whole update runs under a single write lock.
   *
   * @param ctx Context identifier (used for logging, not stored)
   * @param prior_events Buffer contents before the new event was appended
   * @param new_event The newly stored event
   * @param options Temporal / negative-signal options
   */
  void ApplyIngestedEvent(const std::string& ctx, const std::vector<Event>& prior_events, const Event& new_event,
                          const IngestOptions& options);

  /**
   * @brief Get the number of co-occurring neighbors for an item
   * @param item_id Item ID
   * @return Number of neighbors (0 if item not found)
   */
  size_t GetNeighborCount(const std::string& item_id) const;

  /**
   * @brief Get the generation counter (incremented on every write)
   * @return Current generation value
   */
  uint64_t GetGeneration() const { return generation_.load(std::memory_order_acquire); }

  /**
   * @brief Get similar items based on co-occurrence scores
   *
   * Returns the top-k items with highest co-occurrence scores with the
   * given item, sorted by score descending.
   *
   * @param item_id Item ID to find similar items for
   * @param top_k Maximum number of results
   * @return Vector of (item_id, score) pairs, sorted by score descending
   */
  std::vector<std::pair<std::string, float>> GetSimilar(const std::string& item_id, int top_k) const;

  /**
   * @brief Get co-occurrence score between two items
   *
   * @param item_id_1 First item ID
   * @param item_id_2 Second item ID
   * @return Co-occurrence score (0.0 if no co-occurrence)
   */
  float GetScore(const std::string& item_id_1, const std::string& item_id_2) const;

  /**
   * @brief Apply exponential decay to all scores
   *
   * Multiplies all scores by alpha (0.0 < alpha <= 1.0).
   * This favors recent co-occurrences over old ones.
   *
   * @param alpha Decay factor (typically 0.99)
   */
  void ApplyDecay(double alpha);

  /**
   * @brief Get total number of items tracked
   * @return Number of items with at least one co-occurrence
   */
  size_t GetItemCount() const;

  /**
   * @brief Get all item IDs
   * @return Vector of all item IDs
   */
  std::vector<std::string> GetAllItems() const;

  /**
   * @brief Get all stored neighbors for an item, unfiltered
   *
   * Unlike GetSimilar(), this returns every neighbor recorded in the matrix
   * including zero and negative scores, and applies no top-k limit or sorting.
   * It exists for faithful serialization: negative-signal baselines (scores
   * <= 0) must survive a SAVE/LOAD round trip, whereas GetSimilar() filters
   * them out for query results. Do not use this for query paths.
   *
   * @param item_id Item ID whose neighbors to enumerate
   * @return Vector of (neighbor_id, score) pairs in unspecified order (empty if
   *         the item has no recorded neighbors)
   */
  std::vector<std::pair<std::string, float>> GetAllNeighbors(const std::string& item_id) const;

  /**
   * @brief Directly set co-occurrence score between two items
   *
   * Sets the score in both directions (symmetric). This bypasses
   * UpdateFromEvents() and writes directly to the internal matrix.
   * Primarily used for snapshot deserialization to preserve exact scores.
   *
   * @param item1 First item ID
   * @param item2 Second item ID
   * @param score Co-occurrence score to set
   */
  void SetScore(const std::string& item1, const std::string& item2, float score);

  /**
   * @brief Prune the entire index based on config settings
   *
   * Removes entries below min_support and trims neighbors exceeding
   * max_neighbors_per_item. Acquires write lock internally.
   */
  void Prune();

  /**
   * @brief Clear all co-occurrence data
   */
  void Clear();

  /**
   * @brief Get co-occurrence index statistics
   * @return Statistics snapshot
   */
  CoOccurrenceIndexStatistics GetStatistics() const;

  /**
   * @brief Get estimated memory usage in bytes
   * @return Estimated memory usage
   */
  size_t MemoryUsage() const;

  /**
   * @brief Acquire read lock for snapshot consistency
   * @return Shared lock guard
   */
  std::shared_lock<std::shared_mutex> AcquireReadLock() const;

  /**
   * @brief Acquire write lock for snapshot consistency
   * @return Unique lock guard
   */
  std::unique_lock<std::shared_mutex> AcquireWriteLock();

 private:
  // Co-occurrence matrix: id1 -> (id2 -> score)
  // Stored as symmetric matrix (both id1->id2 and id2->id1)
  mutable std::shared_mutex mutex_;
  std::unordered_map<std::string, std::unordered_map<std::string, float>> co_scores_;
  std::atomic<uint64_t> generation_{0};  ///< Generation counter for cache invalidation
  Config config_;                        ///< Pruning configuration

  /// @brief Internal implementation of UpdateFromEvents (no locking)
  void UpdateFromEventsInternal(const std::string& ctx, const std::vector<Event>& events, bool temporal_enabled,
                                double half_life_sec);

  /// @brief Internal implementation of AddEventIncremental (caller holds write lock)
  void AddEventIncrementalInternal(const std::vector<Event>& prior_events, const Event& new_event,
                                   bool temporal_enabled, double half_life_sec);

  /// @brief Prune a single item's neighbor list (caller must hold write lock)
  void PruneItemLocked(const std::string& item_id);

  /// @brief Estimate memory usage assuming the caller already holds the lock
  size_t MemoryUsageLocked() const;
};

}  // namespace nvecd::events
