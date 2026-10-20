/**
 * @file dedup_cache.cpp
 * @brief LRU cache for event deduplication implementation
 */

#include "events/dedup_cache.h"

#include <algorithm>
#include <mutex>

#include "utils/container_memory.h"

namespace nvecd::events {

DedupCache::DedupCache(size_t max_size, uint32_t window_sec) : max_size_(max_size), window_sec_(window_sec) {}

bool DedupCache::IsWithinWindow(const CacheEntry& entry, uint64_t timestamp) const {
  // Event time only: whether a record is a duplicate must depend on the record
  // and the records before it, never on when it arrived, or a WAL replay that
  // applies the same records back to back would diverge from the live run.
  const uint64_t event_time_distance =
      timestamp >= entry.timestamp ? timestamp - entry.timestamp : entry.timestamp - timestamp;
  return event_time_distance <= window_sec_;
}

bool DedupCache::WouldDeduplicate(const EventKey& key, uint64_t current_timestamp) const {
  if (window_sec_ == 0) {
    return false;
  }
  std::shared_lock lock(mutex_);
  const auto it = cache_.find(key);
  if (it == cache_.end()) {
    return false;
  }
  return IsWithinWindow(it->second, current_timestamp);
}

bool DedupCache::CheckAndInsert(const EventKey& key, uint64_t timestamp) {
  if (window_sec_ == 0) {
    total_misses_.fetch_add(1, std::memory_order_relaxed);
    return false;
  }

  std::unique_lock lock(mutex_);
  auto it = cache_.find(key);
  if (it != cache_.end() && IsWithinWindow(it->second, timestamp)) {
    total_hits_.fetch_add(1, std::memory_order_relaxed);
    lru_list_.splice(lru_list_.begin(), lru_list_, it->second.lru_iter);
    return true;
  }

  total_misses_.fetch_add(1, std::memory_order_relaxed);
  RecordLocked(key, timestamp);
  return false;
}

void DedupCache::Seed(const EventKey& key, uint64_t timestamp) {
  if (window_sec_ == 0) {
    return;
  }
  std::unique_lock lock(mutex_);
  RecordLocked(key, timestamp);
}

void DedupCache::RecordLocked(const EventKey& key, uint64_t timestamp) {
  auto it = cache_.find(key);
  if (it != cache_.end()) {
    // A backdated event must not pull the anchor behind a newer accepted one.
    it->second.timestamp = std::max(it->second.timestamp, timestamp);
    lru_list_.splice(lru_list_.begin(), lru_list_, it->second.lru_iter);
    return;
  }

  if (cache_.size() >= max_size_) {
    EvictLRU();
  }
  lru_list_.push_front(key);
  cache_[key] = CacheEntry{timestamp, lru_list_.begin()};
}

size_t DedupCache::MemoryUsage() const {
  std::shared_lock lock(mutex_);
  size_t total = sizeof(*this) + utils::UnorderedBookkeeping(cache_);
  // Each entry holds one key in the map and one in the LRU list.
  for (const auto& [key, entry] : cache_) {
    total += 2 * (sizeof(EventKey) + key.ctx.capacity() + key.id.capacity());
    total += sizeof(CacheEntry) + utils::kListNodeOverhead;
  }
  return total;
}

void DedupCache::Clear() {
  std::unique_lock lock(mutex_);
  cache_.clear();
  lru_list_.clear();
  total_hits_.store(0, std::memory_order_relaxed);
  total_misses_.store(0, std::memory_order_relaxed);
}

size_t DedupCache::Size() const {
  std::shared_lock lock(mutex_);
  return cache_.size();
}

DedupCache::Statistics DedupCache::GetStatistics() const {
  std::shared_lock lock(mutex_);
  Statistics stats;
  stats.size = cache_.size();
  stats.max_size = max_size_;
  stats.total_hits = total_hits_.load(std::memory_order_relaxed);
  stats.total_misses = total_misses_.load(std::memory_order_relaxed);
  return stats;
}

void DedupCache::EvictLRU() {
  // Remove least recently used (back of list)
  if (lru_list_.empty()) {
    return;
  }

  const EventKey& lru_key = lru_list_.back();
  cache_.erase(lru_key);
  lru_list_.pop_back();
}

}  // namespace nvecd::events
