/**
 * @file state_cache.h
 * @brief State cache for SET/DEL event deduplication
 *
 * Tracks the last state of each (ctx, id) pair to enable idempotent
 * state transitions (likes, bookmarks, ratings).
 */

#pragma once

#include <atomic>
#include <list>
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
 * // SET like:item1 to 100 (first time: not a duplicate, state recorded)
 * bool first = cache.CheckAndUpdateSet(key, 100);  // false
 *
 * // SET like:item1 to 100 again (duplicate)
 * if (cache.CheckAndUpdateSet(key, 100)) {
 *   // Skip duplicate
 * }
 *
 * // DEL like:item1
 * if (!cache.CheckAndMarkDeleted(key)) {
 *   // First delete: apply it
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

  /** Check SET duplicate state without updating counters or LRU state. */
  bool WouldDeduplicateSet(const StateKey& key, int score) const;

  /** @brief Atomically deduplicate and record a SET state. */
  bool CheckAndUpdateSet(const StateKey& key, int score);

  /** Check DEL duplicate state without updating counters or LRU state. */
  bool WouldDeduplicateDel(const StateKey& key) const;

  /** @brief Atomically deduplicate and record a DEL state. */
  bool CheckAndMarkDeleted(const StateKey& key);

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

  mutable std::shared_mutex mutex_;  ///< Reader-writer lock

  /// LRU list: most recently used at front
  std::list<StateKey> lru_list_;

  /// Key -> (score, LRU iterator)
  using StateMap = std::unordered_map<StateKey, std::pair<int, std::list<StateKey>::iterator>>;
  StateMap states_;

  // Statistics (atomic for thread-safe access)
  mutable std::atomic<uint64_t> total_hits_{0};
  mutable std::atomic<uint64_t> total_misses_{0};

  /**
   * @brief Evict least recently used entry if cache is full
   * @pre mutex_ is locked for writing
   */
  void EvictIfFull();

  /**
   * @brief Move an existing entry to the front of the LRU list
   * @pre mutex_ is locked for writing; @p it is a valid, non-end iterator
   */
  void TouchLocked(StateMap::iterator it);

  /**
   * @brief Record @p score for @p key and make it most recent, evicting if full
   * @param it Result of states_.find(key)
   * @pre mutex_ is locked for writing
   */
  void UpsertLocked(StateMap::iterator it, const StateKey& key, int score);
};

}  // namespace nvecd::events
