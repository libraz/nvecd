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
#include <unordered_set>
#include <vector>

#include "cache/cache_key.h"
#include "cache/result_compressor.h"
#include "similarity/similarity_result.h"

namespace nvecd::cache {

/**
 * @brief Search type for per-type cache policies
 */
enum class SearchType : uint8_t {
  kItemSearch = 0,      ///< SIM item_id (events/vectors/fusion)
  kVectorSearch = 1,    ///< SIMV query_vector
  kFilteredSearch = 2,  ///< Any search with filter= parameter
};

/// Number of search types
static constexpr size_t kSearchTypeCount = 3;

/**
 * @brief Per-search-type cache policy
 */
struct CachePolicy {
  bool enabled = true;  ///< Whether caching is enabled for this type
  int ttl_seconds = 0;  ///< TTL override (0 = use global TTL)
};

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
  uint64_t ttl_expirations = 0;
  uint64_t decompression_failures = 0;

  // Timing statistics
  double total_cache_hit_time_ms = 0.0;
  double total_cache_miss_time_ms = 0.0;
  double total_query_saved_time_ms = 0.0;

  // Per-search-type statistics
  uint64_t item_search_queries = 0;
  uint64_t item_search_hits = 0;
  uint64_t vector_search_queries = 0;
  uint64_t vector_search_hits = 0;
  uint64_t filtered_search_queries = 0;
  uint64_t filtered_search_hits = 0;

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
  std::atomic<uint64_t> ttl_expirations{0};
  std::atomic<uint64_t> decompression_failures{0};

  // Per-search-type statistics
  std::atomic<uint64_t> per_type_queries[kSearchTypeCount]{};
  std::atomic<uint64_t> per_type_hits[kSearchTypeCount]{};

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
   * @param ttl_seconds TTL for cache entries (0 = no expiration)
   */
  explicit SimilarityCache(size_t max_memory_bytes, double min_query_cost_ms, int ttl_seconds = 0);

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
   * @brief Lookup cache entry with search type policy check
   * @param key Cache key
   * @param search_type Type of search for policy routing
   * @return Cached results if policy allows and entry found, nullopt otherwise
   */
  [[nodiscard]] std::optional<std::vector<similarity::SimilarityResult>> Lookup(const CacheKey& key,
                                                                                SearchType search_type);

  /**
   * @brief Insert cache entry
   * @param key Cache key
   * @param results Search results to cache
   * @param query_cost_ms Query execution time
   * @return true if inserted, false if not cached (below threshold or eviction failure)
   */
  bool Insert(const CacheKey& key, const std::vector<similarity::SimilarityResult>& results, double query_cost_ms);

  /**
   * @brief Insert cache entry with search type policy check
   * @param key Cache key
   * @param results Search results to cache
   * @param query_cost_ms Query execution time
   * @param search_type Type of search for policy routing
   * @return true if inserted, false if policy disables caching for this type
   */
  bool Insert(const CacheKey& key, const std::vector<similarity::SimilarityResult>& results, double query_cost_ms,
              SearchType search_type);

  /**
   * @brief Set cache policy for a search type
   * @param search_type Search type
   * @param policy Cache policy
   */
  void SetSearchTypePolicy(SearchType search_type, const CachePolicy& policy);

  /**
   * @brief Get cache policy for a search type
   * @param search_type Search type
   * @return Current policy
   */
  [[nodiscard]] CachePolicy GetSearchTypePolicy(SearchType search_type) const;

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
   * @brief Register which item IDs appear in a cache entry's results
   *
   * Builds reverse index for selective invalidation. Only tracks
   * up to kMaxTrackedItemsPerEntry items per entry to bound memory.
   *
   * @param key Cache key
   * @param item_ids Item IDs that appear in the query and results
   */
  void RegisterResultItems(const CacheKey& key, const std::vector<std::string>& item_ids);

  /**
   * @brief Invalidate all cache entries that reference a given item ID
   *
   * Uses reverse index for O(k) invalidation where k is the number
   * of affected cache entries, instead of O(n) full cache clear.
   *
   * @param item_id The mutated item ID
   * @return Number of entries invalidated
   */
  size_t InvalidateByItemId(const std::string& item_id);

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
   * @brief Purge all TTL-expired entries
   *
   * Scans all entries and removes those that have exceeded the TTL.
   * Can be called periodically from a background thread.
   *
   * @return Number of entries purged
   */
  size_t PurgeExpired();

  /**
   * @brief Set TTL for cache entries (runtime configuration)
   * @param ttl_seconds TTL in seconds (0 = no expiration)
   */
  void SetTtl(int ttl_seconds) { ttl_seconds_.store(ttl_seconds, std::memory_order_relaxed); }

  /**
   * @brief Get current TTL setting
   * @return TTL in seconds (0 = no expiration)
   */
  [[nodiscard]] int GetTtl() const { return ttl_seconds_.load(std::memory_order_relaxed); }

  /**
   * @brief Set minimum query cost threshold (runtime configuration)
   * @param min_query_cost_ms Minimum query cost in ms
   */
  void SetMinQueryCost(double min_query_cost_ms) {
    min_query_cost_ms_.store(min_query_cost_ms, std::memory_order_relaxed);
  }

  /**
   * @brief Get current minimum query cost threshold
   * @return Minimum query cost in ms
   */
  [[nodiscard]] double GetMinQueryCost() const { return min_query_cost_ms_.load(std::memory_order_relaxed); }

  /**
   * @brief Enable or disable the cache
   * @param enabled Whether the cache should be enabled
   */
  void SetEnabled(bool enabled) { enabled_.store(enabled, std::memory_order_relaxed); }

  /**
   * @brief Check if the cache is enabled
   * @return true if enabled
   */
  [[nodiscard]] bool IsEnabled() const { return enabled_.load(std::memory_order_relaxed); }

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
    snapshot.ttl_expirations = stats_.ttl_expirations.load();
    snapshot.decompression_failures = stats_.decompression_failures.load();
    {
      std::lock_guard<std::mutex> lock(stats_.timing_mutex_);
      snapshot.total_cache_hit_time_ms = stats_.total_cache_hit_time_ms;
      snapshot.total_cache_miss_time_ms = stats_.total_cache_miss_time_ms;
      snapshot.total_query_saved_time_ms = stats_.total_query_saved_time_ms;
    }
    // Per-type stats
    snapshot.item_search_queries = stats_.per_type_queries[0].load();
    snapshot.item_search_hits = stats_.per_type_hits[0].load();
    snapshot.vector_search_queries = stats_.per_type_queries[1].load();
    snapshot.vector_search_hits = stats_.per_type_hits[1].load();
    snapshot.filtered_search_queries = stats_.per_type_queries[2].load();
    snapshot.filtered_search_hits = stats_.per_type_hits[2].load();
    return snapshot;
  }

  friend class SimilarityCacheTestHelper;  // Test-only access to internals

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
    std::vector<std::string> referenced_item_ids;      ///< Item IDs in results (for reverse index cleanup)

    CachedEntry() = default;
    ~CachedEntry() = default;
    CachedEntry(const CachedEntry& other)
        : compressed_data(other.compressed_data),
          original_size(other.original_size),
          query_cost_ms(other.query_cost_ms),
          created_at(other.created_at),
          invalidated(other.invalidated.load()),
          referenced_item_ids(other.referenced_item_ids) {}
    CachedEntry& operator=(const CachedEntry& other) {
      if (this != &other) {
        compressed_data = other.compressed_data;
        original_size = other.original_size;
        query_cost_ms = other.query_cost_ms;
        created_at = other.created_at;
        invalidated.store(other.invalidated.load());
        referenced_item_ids = other.referenced_item_ids;
      }
      return *this;
    }
    CachedEntry(CachedEntry&& other) noexcept
        : compressed_data(std::move(other.compressed_data)),
          original_size(other.original_size),
          query_cost_ms(other.query_cost_ms),
          created_at(other.created_at),
          invalidated(other.invalidated.load()),
          referenced_item_ids(std::move(other.referenced_item_ids)) {}
    CachedEntry& operator=(CachedEntry&& other) noexcept {
      if (this != &other) {
        compressed_data = std::move(other.compressed_data);
        original_size = other.original_size;
        query_cost_ms = other.query_cost_ms;
        created_at = other.created_at;
        invalidated.store(other.invalidated.load());
        referenced_item_ids = std::move(other.referenced_item_ids);
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
  std::atomic<double> min_query_cost_ms_;
  std::atomic<int> ttl_seconds_{0};  ///< TTL in seconds (0 = no expiration)
  std::atomic<bool> enabled_{true};  ///< Cache enabled flag

  /// Reverse index: item_id -> set of CacheKeys containing that item in results
  std::unordered_map<std::string, std::unordered_set<CacheKey>> item_to_cache_keys_;

  /// Maximum item IDs tracked per cache entry for reverse index
  static constexpr size_t kMaxTrackedItemsPerEntry = 50;

  // Per-search-type policies
  mutable std::mutex policy_mutex_;
  CachePolicy policies_[kSearchTypeCount];

  // Memory tracking
  size_t total_memory_bytes_ = 0;

  // Thread safety
  mutable std::shared_mutex mutex_;

  // Statistics
  CacheStatistics stats_;

  /**
   * @brief Erase a cache entry (caller must hold unique_lock on mutex_)
   * @param key Cache key to erase
   * @return true if entry was found and erased
   */
  bool EraseLocked(const CacheKey& key);

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
