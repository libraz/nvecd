/**
 * @file co_occurrence_index.h
 * @brief Co-occurrence scoring index for event-based similarity
 *
 * Tracks which items co-occur in event contexts and provides similarity
 * scores based on co-occurrence patterns.
 */

#pragma once

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
  size_t tracked_ids = 0;       ///< Number of tracked item IDs
  size_t co_pairs = 0;          ///< Number of co-occurrence pairs
  size_t memory_bytes = 0;      ///< Estimated memory usage in bytes
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
   * @brief Construct empty co-occurrence index
   */
  CoOccurrenceIndex() = default;

  /**
   * @brief Update co-occurrence scores from events
   *
   * Computes pairwise co-occurrence scores for all events in the context
   * and adds them to the existing scores.
   *
   * @param ctx Context identifier (used for logging, not stored)
   * @param events Events from a single context
   */
  void UpdateFromEvents(const std::string& ctx,
                        const std::vector<Event>& events);

  /**
   * @brief Get similar items based on co-occurrence scores
   *
   * Returns the top-k items with highest co-occurrence scores with the
   * given item, sorted by score descending.
   *
   * @param id Item ID to find similar items for
   * @param top_k Maximum number of results
   * @return Vector of (item_id, score) pairs, sorted by score descending
   */
  std::vector<std::pair<std::string, float>> GetSimilar(const std::string& id,
                                                         int top_k) const;

  /**
   * @brief Get co-occurrence score between two items
   *
   * @param id1 First item ID
   * @param id2 Second item ID
   * @return Co-occurrence score (0.0 if no co-occurrence)
   */
  float GetScore(const std::string& id1, const std::string& id2) const;

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

 private:
  // Co-occurrence matrix: id1 -> (id2 -> score)
  // Stored as symmetric matrix (both id1->id2 and id2->id1)
  mutable std::shared_mutex mutex_;
  std::unordered_map<std::string, std::unordered_map<std::string, float>>
      co_scores_;
};

}  // namespace nvecd::events
