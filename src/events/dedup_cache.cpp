/**
 * @file dedup_cache.cpp
 * @brief LRU cache for event deduplication implementation
 */

#include "events/dedup_cache.h"

#include <algorithm>
#include <mutex>

namespace nvecd::events {

DedupCache::DedupCache(size_t max_size, uint32_t window_sec) : max_size_(max_size), window_sec_(window_sec) {}

bool DedupCache::IsWithinWindow(const CacheEntry& entry, uint64_t timestamp,
                                std::chrono::steady_clock::time_point now) const {
  const uint64_t event_time_distance =
      timestamp >= entry.timestamp ? timestamp - entry.timestamp : entry.timestamp - timestamp;
  if (event_time_distance <= window_sec_) {
    return true;
  }
  // A client timestamp that jumps backwards beyond the event-time window must
  // not turn every retry into a miss after one future-dated event. Bound that
  // skew by the server's monotonic arrival window; after the real-time window
  // expires, the older event may establish a new event-time baseline.
  return timestamp < entry.timestamp && now - entry.last_seen_at <= std::chrono::seconds(window_sec_);
}

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

  if (IsWithinWindow(it->second, current_timestamp, std::chrono::steady_clock::now())) {
    total_hits_.fetch_add(1, std::memory_order_relaxed);
    return true;  // Duplicate within window
  }

  // Outside window = not a duplicate (will be updated)
  total_misses_.fetch_add(1, std::memory_order_relaxed);
  return false;
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
  return IsWithinWindow(it->second, current_timestamp, std::chrono::steady_clock::now());
}

bool DedupCache::CheckAndInsert(const EventKey& key, uint64_t timestamp) {
  if (window_sec_ == 0) {
    total_misses_.fetch_add(1, std::memory_order_relaxed);
    return false;
  }

  std::unique_lock lock(mutex_);
  const auto now = std::chrono::steady_clock::now();
  auto it = cache_.find(key);
  if (it != cache_.end()) {
    if (IsWithinWindow(it->second, timestamp, now)) {
      total_hits_.fetch_add(1, std::memory_order_relaxed);
      it->second.last_seen_at = now;
      lru_list_.splice(lru_list_.begin(), lru_list_, it->second.lru_iter);
      return true;
    }

    total_misses_.fetch_add(1, std::memory_order_relaxed);
    it->second.timestamp = timestamp;
    it->second.last_seen_at = now;
    lru_list_.splice(lru_list_.begin(), lru_list_, it->second.lru_iter);
    return false;
  }

  total_misses_.fetch_add(1, std::memory_order_relaxed);
  if (cache_.size() >= max_size_) {
    EvictLRU();
  }
  lru_list_.push_front(key);
  cache_[key] = CacheEntry{timestamp, now, lru_list_.begin()};
  return false;
}

void DedupCache::Insert(const EventKey& key, uint64_t timestamp) {
  std::unique_lock lock(mutex_);
  const auto now = std::chrono::steady_clock::now();

  auto it = cache_.find(key);

  if (it != cache_.end()) {
    it->second.timestamp = timestamp;
    it->second.last_seen_at = now;
    lru_list_.splice(lru_list_.begin(), lru_list_, it->second.lru_iter);
    return;
  }

  // New key: check if cache is full
  if (cache_.size() >= max_size_) {
    EvictLRU();
  }

  // Insert new entry
  lru_list_.push_front(key);
  cache_[key] = CacheEntry{timestamp, now, lru_list_.begin()};
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
