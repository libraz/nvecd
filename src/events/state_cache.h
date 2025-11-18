/**
 * @file state_cache.h
 * @brief State cache for SET/DEL event deduplication
 *
 * Tracks the last state of each (ctx, id) pair to enable idempotent
 * state transitions (likes, bookmarks, ratings).
 */

#pragma once

#include <atomic>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>

namespace nvecd::events {

/**
 * @brief State key for deduplication
 */
struct StateKey {
  std::string ctx;  ///< Context identifier
  std::string id;   ///< Event ID

  StateKey(std::string ctx_, std::string id_) : ctx(std::move(ctx_)), id(std::move(id_)) {}

  bool operator==(const StateKey& other) const { return ctx == other.ctx && id == other.id; }
};

}  // namespace nvecd::events

// Hash function for StateKey
namespace std {
template <>
struct hash<nvecd::events::StateKey> {
  size_t operator()(const nvecd::events::StateKey& key) const {
    size_t ctx_hash = std::hash<std::string>{}(key.ctx);
    size_t id_hash = std::hash<std::string>{}(key.id);
    return ctx_hash ^ (id_hash << 1);
  }
};
}  // namespace std

namespace nvecd::events {

/**
 * @brief State cache for SET/DEL event deduplication
 *
 * Tracks the last score for each (ctx, id) to detect duplicate state updates.
 *
 * Example:
 * @code
 * StateCache cache(10000);
 * StateKey key{"user1", "like:item1"};
 *
 * // SET like:item1 to 100
 * if (!cache.IsDuplicateSet(key, 100)) {
 *   cache.UpdateScore(key, 100);
 * }
 *
 * // SET like:item1 to 100 again (duplicate)
 * if (cache.IsDuplicateSet(key, 100)) {
 *   // Skip duplicate
 * }
 *
 * // DEL like:item1
 * if (!cache.IsDuplicateDel(key)) {
 *   cache.MarkDeleted(key);
 * }
 * @endcode
 */
class StateCache {
 public:
  /**
   * @brief Construct state cache
   * @param max_size Maximum number of tracked states (LRU eviction)
   */
  explicit StateCache(size_t max_size);

  /**
   * @brief Check if SET operation is duplicate
   *
   * Returns true if the last score for this key equals the new score.
   *
   * @param key State key
   * @param score New score
   * @return true if duplicate (same score), false otherwise
   */
  bool IsDuplicateSet(const StateKey& key, int score);

  /**
   * @brief Check if DEL operation is duplicate
   *
   * Returns true if this key is already marked as deleted.
   *
   * @param key State key
   * @return true if already deleted, false otherwise
   */
  bool IsDuplicateDel(const StateKey& key);

  /**
   * @brief Update score for a key
   *
   * @param key State key
   * @param score New score
   */
  void UpdateScore(const StateKey& key, int score);

  /**
   * @brief Mark key as deleted
   *
   * @param key State key
   */
  void MarkDeleted(const StateKey& key);

  /**
   * @brief Clear all cached states
   */
  void Clear();

  /**
   * @brief Get current cache size
   * @return Number of tracked states
   */
  size_t Size() const;

  /**
   * @brief Get cache statistics
   */
  struct Statistics {
    size_t size;            ///< Current number of entries
    size_t max_size;        ///< Maximum cache size
    uint64_t total_hits;    ///< Total duplicate detections
    uint64_t total_misses;  ///< Total new states
  };

  Statistics GetStatistics() const;

 private:
  // Special score value for deleted state
  static constexpr int kDeletedScore = -1;

  size_t max_size_;  ///< Maximum cache size

  mutable std::shared_mutex mutex_;           ///< Reader-writer lock
  std::unordered_map<StateKey, int> states_;  ///< Key -> last_score (or kDeletedScore)

  // Statistics (atomic for thread-safe access)
  mutable std::atomic<uint64_t> total_hits_{0};
  mutable std::atomic<uint64_t> total_misses_{0};

  /**
   * @brief Evict oldest entry if cache is full
   * @pre mutex_ is locked for writing
   */
  void EvictIfFull();
};

}  // namespace nvecd::events
