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

SimilarityCache::SimilarityCache(size_t max_memory_bytes, double min_query_cost_ms, int ttl_seconds,
                                 bool compression_enabled, size_t eviction_batch_size)
    : max_memory_bytes_(max_memory_bytes),
      compression_enabled_(compression_enabled),
      eviction_batch_size_(std::max<size_t>(1, eviction_batch_size)),
      min_query_cost_ms_(min_query_cost_ms),
      ttl_seconds_(ttl_seconds) {
  // Default policies: item_search enabled, vector_search disabled, filtered_search enabled with 60s TTL
  policies_[static_cast<size_t>(SearchType::kItemSearch)] = {true, 0};
  policies_[static_cast<size_t>(SearchType::kVectorSearch)] = {false, 0};
  policies_[static_cast<size_t>(SearchType::kFilteredSearch)] = {true, 60};
}

std::chrono::steady_clock::time_point SimilarityCache::ComputeExpiry(int effective_ttl_seconds) {
  if (effective_ttl_seconds <= 0) {
    return std::chrono::steady_clock::time_point::max();
  }
  return std::chrono::steady_clock::now() + std::chrono::seconds(effective_ttl_seconds);
}

std::optional<std::vector<similarity::SimilarityResult>> SimilarityCache::Lookup(const CacheKey& key) {
  return LookupWithTtl(key, ttl_seconds_.load(std::memory_order_relaxed));
}

std::optional<std::vector<similarity::SimilarityResult>> SimilarityCache::LookupWithTtl(const CacheKey& key,
                                                                                        int effective_ttl_seconds) {
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

  // Check TTL expiration.
  // An entry is expired if either its stored absolute expiry (set from the
  // effective TTL at insert time) has passed, or the caller-supplied effective
  // TTL would expire it relative to its creation time. The latter covers a
  // global TTL that was set or lowered after the entry was inserted.
  {
    const auto now = std::chrono::steady_clock::now();
    const auto& checked_entry = iter->second.first;
    bool expired = now >= checked_entry.expires_at;
    if (!expired && effective_ttl_seconds > 0) {
      auto age_seconds = std::chrono::duration_cast<std::chrono::seconds>(now - checked_entry.created_at).count();
      expired = age_seconds >= effective_ttl_seconds;
    }
    if (expired) {
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
  auto decompress_result =
      entry.compressed ? ResultCompressor::DecompressSimilarityResults(entry.compressed_data, entry.original_size)
                       : ResultCompressor::DeserializeSimilarityResults(entry.compressed_data);
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
  return InsertWithTtl(key, results, query_cost_ms, ttl_seconds_.load(std::memory_order_relaxed));
}

bool SimilarityCache::InsertWithTtl(const CacheKey& key, const std::vector<similarity::SimilarityResult>& results,
                                    double query_cost_ms, int effective_ttl_seconds,
                                    const std::vector<std::string>* item_ids) {
  if (!enabled_.load(std::memory_order_relaxed)) {
    return false;
  }

  // Check if query cost meets threshold
  if (query_cost_ms < min_query_cost_ms_.load(std::memory_order_relaxed)) {
    return false;
  }

  auto compress_result = compression_enabled_ ? ResultCompressor::CompressSimilarityResults(results)
                                              : ResultCompressor::SerializeSimilarityResults(results);
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
  entry.compressed = compression_enabled_;
  entry.original_size = original_size;
  entry.query_cost_ms = query_cost_ms;
  entry.created_at = std::chrono::steady_clock::now();
  // Store an absolute expiry from the effective TTL so per-type expiration is
  // honored independently of the global TTL and works in the background purge.
  entry.expires_at = ComputeExpiry(effective_ttl_seconds);
  entry.invalidated.store(false);

  // Insert into LRU list (front = most recent)
  lru_list_.push_front(key);
  auto lru_it = lru_list_.begin();

  // Insert into cache map
  cache_map_.emplace(key, std::make_pair(std::move(entry), lru_it));
  stats_.current_entries++;

  auto inserted = cache_map_.find(key);
  if (item_ids != nullptr) {
    RegisterResultItemsLocked(key, inserted->second.first, *item_ids);
  }

  // Account for cache-map/LRU nodes, entry strings, and the full reverse index.
  RefreshMemoryLocked();
  EvictForSpace(0);
  if (cache_map_.find(key) == cache_map_.end()) {
    // The newly inserted entry could not fit even after evicting older keys.
    return false;
  }

  return true;
}

void SimilarityCache::RegisterResultItems(const CacheKey& key, const std::vector<std::string>& item_ids) {
  std::unique_lock lock(mutex_);

  auto it = cache_map_.find(key);
  if (it == cache_map_.end()) {
    return;  // Entry was evicted before registration
  }

  RegisterResultItemsLocked(key, it->second.first, item_ids);
  RefreshMemoryLocked();
  EvictForSpace(0);
}

void SimilarityCache::RegisterResultItemsLocked(const CacheKey& key, CachedEntry& entry,
                                                const std::vector<std::string>& item_ids) {
  std::unordered_set<std::string> already_registered(entry.referenced_item_ids.begin(),
                                                     entry.referenced_item_ids.end());
  for (const auto& item_id : item_ids) {
    if (entry.referenced_item_ids.size() >= kMaxTrackedItemsPerEntry) {
      break;
    }
    if (!already_registered.insert(item_id).second) {
      continue;
    }
    entry.referenced_item_ids.push_back(item_id);
    item_to_cache_keys_[item_id].insert(key);
  }
}

size_t SimilarityCache::InvalidateByItemId(const std::string& item_id) {
  std::unique_lock lock(mutex_);

  auto rev_it = item_to_cache_keys_.find(item_id);
  if (rev_it == item_to_cache_keys_.end()) {
    return 0;
  }

  // Move keys to avoid iterator invalidation during erasure
  auto keys_to_invalidate = std::move(rev_it->second);
  item_to_cache_keys_.erase(rev_it);

  size_t count = 0;
  for (const auto& cache_key : keys_to_invalidate) {
    if (EraseLocked(cache_key)) {
      ++count;
    }
  }

  return count;
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

  for (const auto& ref_id : iter->second.first.referenced_item_ids) {
    auto reverse = item_to_cache_keys_.find(ref_id);
    if (reverse == item_to_cache_keys_.end()) {
      continue;
    }
    reverse->second.erase(key);
    if (reverse->second.empty()) {
      item_to_cache_keys_.erase(reverse);
    }
  }

  // Remove from LRU list
  lru_list_.erase(iter->second.second);

  stats_.current_entries--;

  // Remove from cache map
  cache_map_.erase(iter);
  RefreshMemoryLocked();

  return true;
}

size_t SimilarityCache::EstimateMemoryLocked() const {
  if (cache_map_.empty() && item_to_cache_keys_.empty()) {
    return 0;
  }

  size_t bytes = cache_map_.bucket_count() * sizeof(void*) + item_to_cache_keys_.bucket_count() * sizeof(void*);
  bytes += lru_list_.size() * (sizeof(CacheKey) + 2 * sizeof(void*));
  for (const auto& [key, entry_pair] : cache_map_) {
    (void)key;
    const auto& entry = entry_pair.first;
    bytes += sizeof(key) + sizeof(entry_pair) + 2 * sizeof(void*) + entry.compressed_data.capacity();
    bytes += entry.referenced_item_ids.capacity() * sizeof(std::string);
    for (const auto& item_id : entry.referenced_item_ids) {
      bytes += item_id.capacity() + 1;
    }
  }
  for (const auto& [item_id, keys] : item_to_cache_keys_) {
    bytes += sizeof(item_id) + sizeof(keys) + 2 * sizeof(void*) + item_id.capacity() + 1;
    bytes += keys.bucket_count() * sizeof(void*);
    bytes += keys.size() * (sizeof(CacheKey) + 2 * sizeof(void*));
  }
  return bytes;
}

void SimilarityCache::RefreshMemoryLocked() {
  total_memory_bytes_ = EstimateMemoryLocked();
  stats_.current_memory_bytes = total_memory_bytes_;
}

void SimilarityCache::Clear() {
  std::unique_lock lock(mutex_);

  lru_list_.clear();
  cache_map_.clear();
  item_to_cache_keys_.clear();
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
    EraseLocked(key);
  }
}

size_t SimilarityCache::PurgeExpired() {
  // The global TTL covers entries inserted with no per-type override. Per-type
  // entries carry their own absolute expiry, so the scan runs even when the
  // global TTL is disabled.
  const int global_ttl = ttl_seconds_.load(std::memory_order_relaxed);

  std::unique_lock lock(mutex_);

  auto now = std::chrono::steady_clock::now();
  std::vector<CacheKey> expired_keys;
  for (const auto& [key, entry_pair] : cache_map_) {
    const auto& entry = entry_pair.first;
    bool expired = now >= entry.expires_at;
    if (!expired && global_ttl > 0) {
      auto age_seconds = std::chrono::duration_cast<std::chrono::seconds>(now - entry.created_at).count();
      expired = age_seconds >= global_ttl;
    }
    if (expired || entry.invalidated.load()) {
      expired_keys.push_back(key);
    }
  }

  for (const auto& key : expired_keys) {
    EraseLocked(key);
  }
  stats_.ttl_expirations += expired_keys.size();

  return expired_keys.size();
}

bool SimilarityCache::EvictForSpace(size_t required_bytes) {
  if (total_memory_bytes_ + required_bytes <= max_memory_bytes_) {
    return true;
  }
  // Evict from LRU tail until enough space is available
  size_t evicted_in_batch = 0;
  while ((total_memory_bytes_ + required_bytes > max_memory_bytes_ || evicted_in_batch < eviction_batch_size_) &&
         !lru_list_.empty()) {
    // Get least recently used key
    const CacheKey lru_key = lru_list_.back();

    auto iter = cache_map_.find(lru_key);
    if (iter == cache_map_.end()) {
      // Inconsistency - remove from LRU list
      lru_list_.pop_back();
      continue;
    }

    EraseLocked(lru_key);
    stats_.evictions++;
    ++evicted_in_batch;
  }

  // Check if enough space was freed
  return total_memory_bytes_ + required_bytes <= max_memory_bytes_;
}

std::optional<std::vector<similarity::SimilarityResult>> SimilarityCache::Lookup(const CacheKey& key,
                                                                                 SearchType search_type) {
  auto idx = static_cast<size_t>(search_type);

  // Check per-type policy and compute the effective TTL without mutating shared
  // state. A positive per-type override wins, otherwise use the global TTL.
  int effective_ttl = ttl_seconds_.load(std::memory_order_relaxed);
  {
    std::lock_guard<std::mutex> lock(policy_mutex_);
    if (!policies_[idx].enabled) {
      return std::nullopt;
    }
    if (policies_[idx].ttl_seconds > 0) {
      effective_ttl = policies_[idx].ttl_seconds;
    }
  }

  stats_.per_type_queries[idx]++;

  auto result = LookupWithTtl(key, effective_ttl);

  if (result.has_value()) {
    stats_.per_type_hits[idx]++;
  }

  return result;
}

bool SimilarityCache::Insert(const CacheKey& key, const std::vector<similarity::SimilarityResult>& results,
                             double query_cost_ms, SearchType search_type) {
  auto idx = static_cast<size_t>(search_type);

  // Check per-type policy and compute the effective TTL without mutating shared
  // state. A positive per-type override wins, otherwise use the global TTL.
  int effective_ttl = ttl_seconds_.load(std::memory_order_relaxed);
  {
    std::lock_guard<std::mutex> lock(policy_mutex_);
    if (!policies_[idx].enabled) {
      return false;
    }
    if (policies_[idx].ttl_seconds > 0) {
      effective_ttl = policies_[idx].ttl_seconds;
    }
  }

  return InsertWithTtl(key, results, query_cost_ms, effective_ttl);
}

bool SimilarityCache::InsertAndRegister(const CacheKey& key, const std::vector<similarity::SimilarityResult>& results,
                                        const std::vector<std::string>& item_ids, double query_cost_ms,
                                        SearchType search_type) {
  const auto index = static_cast<size_t>(search_type);
  int effective_ttl = ttl_seconds_.load(std::memory_order_relaxed);
  {
    std::lock_guard<std::mutex> lock(policy_mutex_);
    if (!policies_[index].enabled) {
      return false;
    }
    if (policies_[index].ttl_seconds > 0) {
      effective_ttl = policies_[index].ttl_seconds;
    }
  }
  return InsertWithTtl(key, results, query_cost_ms, effective_ttl, &item_ids);
}

void SimilarityCache::SetSearchTypePolicy(SearchType search_type, const CachePolicy& policy) {
  std::lock_guard<std::mutex> lock(policy_mutex_);
  policies_[static_cast<size_t>(search_type)] = policy;
}

CachePolicy SimilarityCache::GetSearchTypePolicy(SearchType search_type) const {
  std::lock_guard<std::mutex> lock(policy_mutex_);
  return policies_[static_cast<size_t>(search_type)];
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
