/**
 * @file dedup_cache.cpp
 * @brief LRU cache for event deduplication implementation
 */

#include "events/dedup_cache.h"

#include <algorithm>
#include <mutex>

namespace nvecd::events {

DedupCache::DedupCache(size_t max_size, uint32_t window_sec) : max_size_(max_size), window_sec_(window_sec) {}

bool DedupCache::IsDuplicate(const EventKey& key, uint64_t current_timestamp) const {
  // Window of 0 means deduplication is disabled
  if (window_sec_ == 0) {
    total_misses_.fetch_add(1, std::memory_order_relaxed);
    return false;
  }

  std::shared_lock lock(mutex_);

  auto it = cache_.find(key);
  if (it == cache_.end()) {
    total_misses_.fetch_add(1, std::memory_order_relaxed);
    return false;  // Not in cache = new event
  }

  // Check if within time window
  uint64_t prev_timestamp = it->second.timestamp;
  if (current_timestamp >= prev_timestamp && (current_timestamp - prev_timestamp) <= window_sec_) {
    total_hits_.fetch_add(1, std::memory_order_relaxed);
    return true;  // Duplicate within window
  }

  // Outside window = not a duplicate (will be updated)
  total_misses_.fetch_add(1, std::memory_order_relaxed);
  return false;
}

bool DedupCache::CheckAndInsert(const EventKey& key, uint64_t timestamp) {
  if (window_sec_ == 0) {
    total_misses_.fetch_add(1, std::memory_order_relaxed);
    return false;
  }

  std::unique_lock lock(mutex_);
  auto it = cache_.find(key);
  if (it != cache_.end()) {
    const uint64_t previous = it->second.timestamp;
    if (timestamp >= previous && timestamp - previous <= window_sec_) {
      total_hits_.fetch_add(1, std::memory_order_relaxed);
      return true;
    }

    total_misses_.fetch_add(1, std::memory_order_relaxed);
    it->second.timestamp = std::max(previous, timestamp);
    lru_list_.splice(lru_list_.begin(), lru_list_, it->second.lru_iter);
    return false;
  }

  total_misses_.fetch_add(1, std::memory_order_relaxed);
  if (cache_.size() >= max_size_) {
    EvictLRU();
  }
  lru_list_.push_front(key);
  cache_[key] = CacheEntry{timestamp, lru_list_.begin()};
  return false;
}

void DedupCache::Insert(const EventKey& key, uint64_t timestamp) {
  std::unique_lock lock(mutex_);

  auto it = cache_.find(key);

  if (it != cache_.end()) {
    // Key exists: keep the latest seen timestamp and move to front of LRU.
    // Using max() guards against out-of-order (non-monotonic) client
    // timestamps: an earlier-than-stored timestamp must not roll back the
    // dedup window, otherwise subsequent in-window events could slip through.
    it->second.timestamp = std::max(it->second.timestamp, timestamp);
    lru_list_.splice(lru_list_.begin(), lru_list_, it->second.lru_iter);
    return;
  }

  // New key: check if cache is full
  if (cache_.size() >= max_size_) {
    EvictLRU();
  }

  // Insert new entry
  lru_list_.push_front(key);
  cache_[key] = CacheEntry{timestamp, lru_list_.begin()};
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
