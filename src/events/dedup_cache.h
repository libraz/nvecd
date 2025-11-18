/**
 * @file dedup_cache.h
 * @brief LRU cache for event deduplication
 *
 * Lightweight LRU cache to track recent events and prevent duplicate processing
 * within a configurable time window.
 *
 * Design:
 * - Fixed-size LRU cache with O(1) lookup and insertion
 * - Key: (ctx, id, score) tuple
 * - Value: timestamp of last seen event
 * - Thread-safe with shared_mutex (reader-writer lock)
 */

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <list>
#include <shared_mutex>
#include <string>
#include <unordered_map>

namespace nvecd::events {

/**
 * @brief Event key for deduplication
 */
struct EventKey {
  std::string ctx;  ///< Context identifier
  std::string id;   ///< Event ID
  int score;        ///< Event score

  EventKey(std::string ctx_, std::string id_, int score_) : ctx(std::move(ctx_)), id(std::move(id_)), score(score_) {}

  bool operator==(const EventKey& other) const { return ctx == other.ctx && id == other.id && score == other.score; }
};

}  // namespace nvecd::events

// Hash function for EventKey
namespace std {
template <>
struct hash<nvecd::events::EventKey> {
  size_t operator()(const nvecd::events::EventKey& key) const {
    // Combine hashes using FNV-1a-like algorithm
    size_t h1 = std::hash<std::string>{}(key.ctx);
    size_t h2 = std::hash<std::string>{}(key.id);
    size_t h3 = std::hash<int>{}(key.score);
    return h1 ^ (h2 << 1) ^ (h3 << 2);
  }
};
}  // namespace std

namespace nvecd::events {

/**
 * @brief LRU cache for event deduplication
 *
 * Thread-safe LRU cache that tracks recent events to detect duplicates
 * within a time window.
 *
 * Example:
 * @code
 * DedupCache cache(10000, 60);  // 10k entries, 60 second window
 * EventKey key{"user123", "item456", 95};
 * if (cache.IsDuplicate(key, current_timestamp)) {
 *   // Skip duplicate event
 * } else {
 *   // Process new event
 *   cache.Insert(key, current_timestamp);
 * }
 * @endcode
 */
class DedupCache {
 public:
  /**
   * @brief Construct deduplication cache
   * @param max_size Maximum number of entries (LRU eviction)
   * @param window_sec Time window in seconds for duplicate detection
   */
  DedupCache(size_t max_size, uint32_t window_sec);

  /**
   * @brief Check if event is a duplicate within time window
   *
   * An event is considered duplicate if:
   * 1. Same (ctx, id, score) exists in cache
   * 2. Previous timestamp is within window_sec from current_timestamp
   *
   * @param key Event key
   * @param current_timestamp Current event timestamp (seconds since epoch)
   * @return true if duplicate, false otherwise
   */
  bool IsDuplicate(const EventKey& key, uint64_t current_timestamp) const;

  /**
   * @brief Insert event into cache
   *
   * If cache is full, evicts least recently used entry.
   * If key already exists, updates timestamp and moves to front.
   *
   * @param key Event key
   * @param timestamp Event timestamp (seconds since epoch)
   */
  void Insert(const EventKey& key, uint64_t timestamp);

  /**
   * @brief Clear all entries from cache
   */
  void Clear();

  /**
   * @brief Get current cache size
   * @return Number of entries in cache
   */
  size_t Size() const;

  /**
   * @brief Get cache statistics
   */
  struct Statistics {
    size_t size;            ///< Current number of entries
    size_t max_size;        ///< Maximum cache size
    uint64_t total_hits;    ///< Total number of duplicate detections
    uint64_t total_misses;  ///< Total number of new events
  };

  Statistics GetStatistics() const;

 private:
  // LRU list: most recent at front, least recent at back
  using LRUList = std::list<EventKey>;
  using LRUIterator = LRUList::iterator;

  // Cache entry: timestamp + iterator to LRU list
  struct CacheEntry {
    uint64_t timestamp;
    LRUIterator lru_iter;
  };

  size_t max_size_;      ///< Maximum cache size
  uint32_t window_sec_;  ///< Time window in seconds

  mutable std::shared_mutex mutex_;                 ///< Reader-writer lock
  LRUList lru_list_;                                ///< LRU list (front = most recent)
  std::unordered_map<EventKey, CacheEntry> cache_;  ///< Key -> Entry map

  // Statistics (atomic for thread-safe const access)
  mutable std::atomic<uint64_t> total_hits_{0};
  mutable std::atomic<uint64_t> total_misses_{0};

  /**
   * @brief Evict least recently used entry
   * @pre mutex_ is locked for writing
   */
  void EvictLRU();
};

}  // namespace nvecd::events
