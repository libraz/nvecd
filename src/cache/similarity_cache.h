/**
 * @file similarity_cache.h
 * @brief LRU cache for similarity search results
 *
 * Reference: ../mygram-db/src/cache/query_cache.h
 * Reusability: 90% (adapted for SIM/SIMV results instead of DocId)
 * Adapted for: nvecd similarity search caching
 */

#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "cache/cache_key.h"
#include "cache/result_compressor.h"
#include "similarity/similarity_engine.h"

namespace nvecd::cache {

/**
 * @brief Cache statistics snapshot (copyable)
 *
 * Snapshot of cache statistics for reporting.
 */
struct CacheStatisticsSnapshot {
  // Query statistics
  uint64_t total_queries = 0;
  uint64_t cache_hits = 0;
  uint64_t cache_misses = 0;
  uint64_t cache_misses_invalidated = 0;
  uint64_t cache_misses_not_found = 0;

  // Memory statistics
  uint64_t current_entries = 0;
  uint64_t current_memory_bytes = 0;
  uint64_t evictions = 0;

  // Timing statistics
  double total_cache_hit_time_ms = 0.0;
  double total_cache_miss_time_ms = 0.0;
  double total_query_saved_time_ms = 0.0;

  /**
   * @brief Calculate cache hit rate
   */
  [[nodiscard]] double HitRate() const {
    return total_queries > 0 ? static_cast<double>(cache_hits) / static_cast<double>(total_queries) : 0.0;
  }

  /**
   * @brief Calculate average cache hit latency
   */
  [[nodiscard]] double AverageCacheHitLatency() const {
    return cache_hits > 0 ? total_cache_hit_time_ms / static_cast<double>(cache_hits) : 0.0;
  }

  /**
   * @brief Calculate average cache miss latency
   */
  [[nodiscard]] double AverageCacheMissLatency() const {
    return cache_misses > 0 ? total_cache_miss_time_ms / static_cast<double>(cache_misses) : 0.0;
  }

  /**
   * @brief Get total time saved by cache hits
   */
  [[nodiscard]] double TotalTimeSaved() const { return total_query_saved_time_ms; }
};

/**
 * @brief Internal cache statistics (thread-safe)
 */
struct CacheStatistics {
  // Query statistics
  std::atomic<uint64_t> total_queries{0};
  std::atomic<uint64_t> cache_hits{0};
  std::atomic<uint64_t> cache_misses{0};
  std::atomic<uint64_t> cache_misses_invalidated{0};
  std::atomic<uint64_t> cache_misses_not_found{0};

  // Memory statistics
  std::atomic<uint64_t> current_entries{0};
  std::atomic<uint64_t> current_memory_bytes{0};
  std::atomic<uint64_t> evictions{0};

  // Timing statistics (protected by mutex)
  mutable std::mutex timing_mutex_;
  double total_cache_hit_time_ms{0.0};
  double total_cache_miss_time_ms{0.0};
  double total_query_saved_time_ms{0.0};
};

/**
 * @brief LRU cache for similarity search results
 *
 * Thread-safe cache with LRU eviction policy.
 * Caches results for SIM (ID-based) and SIMV (vector-based) queries.
 */
class SimilarityCache {
 public:
  /**
   * @brief Constructor
   * @param max_memory_bytes Maximum memory usage in bytes
   * @param min_query_cost_ms Minimum query cost to cache (ms)
   */
  explicit SimilarityCache(size_t max_memory_bytes, double min_query_cost_ms);

  /**
   * @brief Destructor
   */
  ~SimilarityCache() = default;

  // Non-copyable, non-movable
  SimilarityCache(const SimilarityCache&) = delete;
  SimilarityCache& operator=(const SimilarityCache&) = delete;
  SimilarityCache(SimilarityCache&&) = delete;
  SimilarityCache& operator=(SimilarityCache&&) = delete;

  /**
   * @brief Lookup cache entry
   * @param key Cache key
   * @return Cached results if found and not invalidated, nullopt otherwise
   */
  [[nodiscard]] std::optional<std::vector<similarity::SimilarityResult>> Lookup(const CacheKey& key);

  /**
   * @brief Insert cache entry
   * @param key Cache key
   * @param results Search results to cache
   * @param query_cost_ms Query execution time
   * @return true if inserted, false if not cached (below threshold or eviction failure)
   */
  bool Insert(const CacheKey& key, const std::vector<similarity::SimilarityResult>& results, double query_cost_ms);

  /**
   * @brief Mark cache entry as invalidated (immediate invalidation)
   * @param key Cache key
   * @return true if entry was found and marked
   */
  bool MarkInvalidated(const CacheKey& key);

  /**
   * @brief Erase cache entry (deferred invalidation)
   * @param key Cache key
   * @return true if entry was found and erased
   */
  bool Erase(const CacheKey& key);

  /**
   * @brief Clear all cache entries
   */
  void Clear();

  /**
   * @brief Clear cache entries matching a predicate
   * @param predicate Function returning true for keys to erase
   */
  void ClearIf(std::function<bool(const CacheKey&)> predicate);

  /**
   * @brief Get cache statistics snapshot (thread-safe)
   */
  [[nodiscard]] CacheStatisticsSnapshot GetStatistics() const {
    CacheStatisticsSnapshot snapshot;
    snapshot.total_queries = stats_.total_queries.load();
    snapshot.cache_hits = stats_.cache_hits.load();
    snapshot.cache_misses = stats_.cache_misses.load();
    snapshot.cache_misses_invalidated = stats_.cache_misses_invalidated.load();
    snapshot.cache_misses_not_found = stats_.cache_misses_not_found.load();
    snapshot.current_entries = stats_.current_entries.load();
    snapshot.current_memory_bytes = stats_.current_memory_bytes.load();
    snapshot.evictions = stats_.evictions.load();
    {
      std::lock_guard<std::mutex> lock(stats_.timing_mutex_);
      snapshot.total_cache_hit_time_ms = stats_.total_cache_hit_time_ms;
      snapshot.total_cache_miss_time_ms = stats_.total_cache_miss_time_ms;
      snapshot.total_query_saved_time_ms = stats_.total_query_saved_time_ms;
    }
    return snapshot;
  }

 private:
  /**
   * @brief Cached entry data structure
   */
  struct CachedEntry {
    std::vector<uint8_t> compressed_data;              ///< Compressed result data
    size_t original_size = 0;                          ///< Original size before compression
    double query_cost_ms = 0.0;                        ///< Original query execution time
    std::chrono::steady_clock::time_point created_at;  ///< Creation timestamp
    std::atomic<bool> invalidated{false};              ///< Invalidation flag (lock-free)

    CachedEntry() = default;
    ~CachedEntry() = default;
    CachedEntry(const CachedEntry& other)
        : compressed_data(other.compressed_data),
          original_size(other.original_size),
          query_cost_ms(other.query_cost_ms),
          created_at(other.created_at),
          invalidated(other.invalidated.load()) {}
    CachedEntry& operator=(const CachedEntry& other) {
      if (this != &other) {
        compressed_data = other.compressed_data;
        original_size = other.original_size;
        query_cost_ms = other.query_cost_ms;
        created_at = other.created_at;
        invalidated.store(other.invalidated.load());
      }
      return *this;
    }
    CachedEntry(CachedEntry&& other) noexcept
        : compressed_data(std::move(other.compressed_data)),
          original_size(other.original_size),
          query_cost_ms(other.query_cost_ms),
          created_at(other.created_at),
          invalidated(other.invalidated.load()) {}
    CachedEntry& operator=(CachedEntry&& other) noexcept {
      if (this != &other) {
        compressed_data = std::move(other.compressed_data);
        original_size = other.original_size;
        query_cost_ms = other.query_cost_ms;
        created_at = other.created_at;
        invalidated.store(other.invalidated.load());
      }
      return *this;
    }
  };

  // LRU list: most recently used at front
  std::list<CacheKey> lru_list_;

  // Map: cache key -> (cached entry, LRU iterator)
  std::unordered_map<CacheKey, std::pair<CachedEntry, std::list<CacheKey>::iterator>> cache_map_;

  // Configuration
  size_t max_memory_bytes_;
  double min_query_cost_ms_;

  // Memory tracking
  size_t total_memory_bytes_ = 0;

  // Thread safety
  mutable std::shared_mutex mutex_;

  // Statistics
  CacheStatistics stats_;

  /**
   * @brief Evict entries to make room for new entry
   * @param required_bytes Bytes needed for new entry
   * @return true if enough space was freed
   */
  bool EvictForSpace(size_t required_bytes);

  /**
   * @brief Move key to front of LRU list (most recently used)
   */
  void Touch(const CacheKey& key);
};

}  // namespace nvecd::cache
