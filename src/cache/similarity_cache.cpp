/**
 * @file similarity_cache.cpp
 * @brief LRU cache for similarity search results implementation
 *
 * Reference: ../mygram-db/src/cache/query_cache.cpp
 * Reusability: 90% (adapted for SIM/SIMV results instead of DocId)
 * Adapted for: nvecd similarity search caching
 */

#include "cache/similarity_cache.h"

#include <algorithm>
#include <chrono>
#include <cstdint>

namespace nvecd::cache {

// Forward declaration from result_compressor.cpp
// Must match SerializedSimilarityResult size for compression/decompression
namespace {
// Must match SerializedSimilarityResult layout: char id[256] + float score
constexpr size_t kSerializedResultSize = 256 + sizeof(float);
}  // namespace

SimilarityCache::SimilarityCache(size_t max_memory_bytes, double min_query_cost_ms, int ttl_seconds)
    : max_memory_bytes_(max_memory_bytes), min_query_cost_ms_(min_query_cost_ms), ttl_seconds_(ttl_seconds) {}

std::optional<std::vector<similarity::SimilarityResult>> SimilarityCache::Lookup(const CacheKey& key) {
  if (!enabled_.load(std::memory_order_relaxed)) {
    return std::nullopt;
  }

  // Start timing
  auto start_time = std::chrono::high_resolution_clock::now();

  // Shared lock for read
  std::shared_lock lock(mutex_);

  stats_.total_queries++;

  auto iter = cache_map_.find(key);
  if (iter == cache_map_.end()) {
    stats_.cache_misses++;
    stats_.cache_misses_not_found++;

    // Record miss latency
    auto end_time = std::chrono::high_resolution_clock::now();
    double miss_time_ms = std::chrono::duration<double, std::milli>(end_time - start_time).count();
    {
      std::lock_guard<std::mutex> timing_lock(stats_.timing_mutex_);
      stats_.total_cache_miss_time_ms += miss_time_ms;
    }

    return std::nullopt;
  }

  // Check invalidation flag
  if (iter->second.first.invalidated.load()) {
    stats_.cache_misses++;
    stats_.cache_misses_invalidated++;

    // Record miss latency
    auto end_time = std::chrono::high_resolution_clock::now();
    double miss_time_ms = std::chrono::duration<double, std::milli>(end_time - start_time).count();
    {
      std::lock_guard<std::mutex> timing_lock(stats_.timing_mutex_);
      stats_.total_cache_miss_time_ms += miss_time_ms;
    }

    return std::nullopt;
  }

  // Check TTL expiration
  const int ttl = ttl_seconds_.load(std::memory_order_relaxed);
  if (ttl > 0) {
    auto now = std::chrono::steady_clock::now();
    auto age_seconds = std::chrono::duration_cast<std::chrono::seconds>(now - iter->second.first.created_at).count();
    if (age_seconds >= ttl) {
      // Entry has expired - treat as miss
      stats_.cache_misses++;
      stats_.cache_misses_not_found++;
      stats_.ttl_expirations++;

      // Save created_at for verification after lock upgrade
      auto expired_created_at = iter->second.first.created_at;

      // Release shared lock, acquire unique lock to erase expired entry
      lock.unlock();
      {
        std::unique_lock write_lock(mutex_);
        auto recheck_iter = cache_map_.find(key);
        if (recheck_iter != cache_map_.end() && recheck_iter->second.first.created_at == expired_created_at) {
          EraseLocked(key);
        }
      }

      // Record miss latency
      auto end_time = std::chrono::high_resolution_clock::now();
      double miss_time_ms = std::chrono::duration<double, std::milli>(end_time - start_time).count();
      {
        std::lock_guard<std::mutex> timing_lock(stats_.timing_mutex_);
        stats_.total_cache_miss_time_ms += miss_time_ms;
      }

      return std::nullopt;
    }
  }

  // Decompress result and copy query_cost_ms before releasing lock
  const auto& entry = iter->second.first;
  auto entry_created_at = entry.created_at;
  auto decompress_result = ResultCompressor::DecompressSimilarityResults(entry.compressed_data, entry.original_size);
  if (!decompress_result.has_value()) {
    // Decompression failed, treat as miss
    stats_.cache_misses++;
    stats_.cache_misses_not_found++;
    stats_.decompression_failures++;

    // Release shared lock, acquire unique lock to erase corrupted entry
    lock.unlock();
    {
      std::unique_lock write_lock(mutex_);
      auto recheck_iter = cache_map_.find(key);
      if (recheck_iter != cache_map_.end() && recheck_iter->second.first.created_at == entry_created_at) {
        EraseLocked(key);
      }
    }

    // Record miss latency
    auto end_time = std::chrono::high_resolution_clock::now();
    double miss_time_ms = std::chrono::duration<double, std::milli>(end_time - start_time).count();
    {
      std::lock_guard<std::mutex> timing_lock(stats_.timing_mutex_);
      stats_.total_cache_miss_time_ms += miss_time_ms;
    }

    return std::nullopt;
  }
  auto result = std::move(*decompress_result);

  // Cache hit (counted after successful decompression)
  stats_.cache_hits++;

  // Copy query_cost_ms before releasing lock to avoid use-after-free
  const double query_cost_ms = entry.query_cost_ms;

  // Update access time (need to upgrade to unique lock)
  lock.unlock();
  std::unique_lock write_lock(mutex_);

  // Re-check existence and verify it's the same entry (not a new entry with same key)
  iter = cache_map_.find(key);
  if (iter != cache_map_.end() && iter->second.first.created_at == entry_created_at) {
    Touch(key);

    // Record hit latency and saved time
    auto end_time = std::chrono::high_resolution_clock::now();
    double hit_time_ms = std::chrono::duration<double, std::milli>(end_time - start_time).count();
    {
      std::lock_guard<std::mutex> timing_lock(stats_.timing_mutex_);
      stats_.total_cache_hit_time_ms += hit_time_ms;
      stats_.total_query_saved_time_ms += query_cost_ms;
    }
  }

  return result;
}

bool SimilarityCache::Insert(const CacheKey& key, const std::vector<similarity::SimilarityResult>& results,
                             double query_cost_ms) {
  if (!enabled_.load(std::memory_order_relaxed)) {
    return false;
  }

  // Check if query cost meets threshold
  if (query_cost_ms < min_query_cost_ms_.load(std::memory_order_relaxed)) {
    return false;
  }

  // Compress result
  auto compress_result = ResultCompressor::CompressSimilarityResults(results);
  if (!compress_result.has_value()) {
    return false;
  }
  auto compressed = std::move(*compress_result);

  // Calculate original size in bytes (for decompression)
  // Must use serialized size, not sizeof(SimilarityResult) which includes std::string overhead
  // Guard against integer overflow in size calculation
  if (results.size() > SIZE_MAX / kSerializedResultSize) {
    return false;
  }
  const size_t original_size = results.size() * kSerializedResultSize;

  // Calculate entry memory usage
  // Entry overhead + compressed data + key size
  const size_t entry_memory = sizeof(CachedEntry) + compressed.capacity() + sizeof(CacheKey);

  // Don't cache if entry is too large
  if (entry_memory > max_memory_bytes_) {
    return false;
  }

  // Exclusive lock for write
  std::unique_lock lock(mutex_);

  // Check if already exists
  if (cache_map_.find(key) != cache_map_.end()) {
    return false;
  }

  // Evict entries if needed
  if (total_memory_bytes_ + entry_memory > max_memory_bytes_) {
    if (!EvictForSpace(entry_memory)) {
      return false;
    }
  }

  // Create cache entry
  CachedEntry entry;
  entry.compressed_data = std::move(compressed);
  entry.original_size = original_size;
  entry.query_cost_ms = query_cost_ms;
  entry.created_at = std::chrono::steady_clock::now();
  entry.invalidated.store(false);

  // Insert into LRU list (front = most recent)
  lru_list_.push_front(key);
  auto lru_it = lru_list_.begin();

  // Insert into cache map
  cache_map_.emplace(key, std::make_pair(std::move(entry), lru_it));

  // Update memory tracking
  total_memory_bytes_ += entry_memory;
  stats_.current_entries++;
  stats_.current_memory_bytes = total_memory_bytes_;

  return true;
}

bool SimilarityCache::MarkInvalidated(const CacheKey& key) {
  std::shared_lock lock(mutex_);

  auto iter = cache_map_.find(key);
  if (iter == cache_map_.end()) {
    return false;
  }

  // Atomic flag set (no lock upgrade needed)
  iter->second.first.invalidated.store(true);

  return true;
}

bool SimilarityCache::Erase(const CacheKey& key) {
  std::unique_lock lock(mutex_);
  return EraseLocked(key);
}

bool SimilarityCache::EraseLocked(const CacheKey& key) {
  auto iter = cache_map_.find(key);
  if (iter == cache_map_.end()) {
    return false;
  }

  // Calculate entry memory before removal
  const size_t entry_memory = sizeof(CachedEntry) + iter->second.first.compressed_data.capacity() + sizeof(CacheKey);

  // Remove from LRU list
  lru_list_.erase(iter->second.second);

  // Update memory tracking
  total_memory_bytes_ -= entry_memory;
  stats_.current_entries--;
  stats_.current_memory_bytes = total_memory_bytes_;

  // Remove from cache map
  cache_map_.erase(iter);

  return true;
}

void SimilarityCache::Clear() {
  std::unique_lock lock(mutex_);

  lru_list_.clear();
  cache_map_.clear();
  total_memory_bytes_ = 0;
  stats_.current_entries = 0;
  stats_.current_memory_bytes = 0;
}

void SimilarityCache::ClearIf(std::function<bool(const CacheKey&)> predicate) {
  std::unique_lock lock(mutex_);

  // Find all entries matching predicate
  std::vector<CacheKey> to_erase;
  for (const auto& [key, entry_pair] : cache_map_) {
    if (predicate(key)) {
      to_erase.push_back(key);
    }
  }

  // Erase entries
  for (const auto& key : to_erase) {
    auto iter = cache_map_.find(key);
    if (iter != cache_map_.end()) {
      // Calculate entry memory
      const size_t entry_memory =
          sizeof(CachedEntry) + iter->second.first.compressed_data.capacity() + sizeof(CacheKey);

      // Remove from LRU list
      lru_list_.erase(iter->second.second);

      // Update memory tracking
      total_memory_bytes_ -= entry_memory;
      stats_.current_entries--;

      // Remove from cache map
      cache_map_.erase(iter);
    }
  }

  stats_.current_memory_bytes = total_memory_bytes_;
}

size_t SimilarityCache::PurgeExpired() {
  const int ttl = ttl_seconds_.load(std::memory_order_relaxed);
  if (ttl <= 0) {
    return 0;  // No TTL configured
  }

  std::unique_lock lock(mutex_);

  auto now = std::chrono::steady_clock::now();
  size_t purged = 0;

  for (auto it = cache_map_.begin(); it != cache_map_.end();) {
    auto age_seconds = std::chrono::duration_cast<std::chrono::seconds>(now - it->second.first.created_at).count();
    if (age_seconds >= ttl || it->second.first.invalidated.load()) {
      // Calculate entry memory before removal
      const size_t entry_memory = sizeof(CachedEntry) + it->second.first.compressed_data.capacity() + sizeof(CacheKey);

      // Remove from LRU list
      lru_list_.erase(it->second.second);

      // Update memory tracking
      total_memory_bytes_ -= entry_memory;
      stats_.current_entries--;

      it = cache_map_.erase(it);
      ++purged;
    } else {
      ++it;
    }
  }

  if (purged > 0) {
    stats_.current_memory_bytes = total_memory_bytes_;
    stats_.ttl_expirations += purged;
  }

  return purged;
}

bool SimilarityCache::EvictForSpace(size_t required_bytes) {
  // Evict from LRU tail until enough space is available
  while (total_memory_bytes_ + required_bytes > max_memory_bytes_ && !lru_list_.empty()) {
    // Get least recently used key
    const CacheKey lru_key = lru_list_.back();

    auto iter = cache_map_.find(lru_key);
    if (iter == cache_map_.end()) {
      // Inconsistency - remove from LRU list
      lru_list_.pop_back();
      continue;
    }

    // Calculate entry memory
    const size_t entry_memory = sizeof(CachedEntry) + iter->second.first.compressed_data.capacity() + sizeof(CacheKey);

    // Remove entry
    lru_list_.pop_back();
    cache_map_.erase(iter);

    // Update memory tracking
    total_memory_bytes_ -= entry_memory;
    stats_.current_entries--;
    stats_.evictions++;
  }

  stats_.current_memory_bytes = total_memory_bytes_;

  // Check if enough space was freed
  return total_memory_bytes_ + required_bytes <= max_memory_bytes_;
}

void SimilarityCache::Touch(const CacheKey& key) {
  auto iter = cache_map_.find(key);
  if (iter == cache_map_.end()) {
    return;
  }

  // Move to front of LRU list
  lru_list_.erase(iter->second.second);
  lru_list_.push_front(key);
  iter->second.second = lru_list_.begin();
}

}  // namespace nvecd::cache
