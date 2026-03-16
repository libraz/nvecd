/**
 * @file state_cache.cpp
 * @brief State cache implementation
 */

#include "events/state_cache.h"

#include <mutex>

namespace nvecd::events {

StateCache::StateCache(size_t max_size) : max_size_(max_size) {}

bool StateCache::IsDuplicateSet(const StateKey& key, int score) {
  std::shared_lock lock(mutex_);

  auto it = states_.find(key);
  if (it == states_.end()) {
    total_misses_.fetch_add(1, std::memory_order_relaxed);
    return false;  // New key
  }

  // Check if last score equals new score
  if (it->second.first == score) {
    total_hits_.fetch_add(1, std::memory_order_relaxed);
    return true;  // Duplicate SET
  }

  total_misses_.fetch_add(1, std::memory_order_relaxed);
  return false;  // State transition
}

bool StateCache::IsDuplicateDel(const StateKey& key) {
  std::shared_lock lock(mutex_);

  auto it = states_.find(key);
  if (it == states_.end()) {
    total_misses_.fetch_add(1, std::memory_order_relaxed);
    return false;  // Not yet tracked
  }

  // Check if already deleted
  if (it->second.first == kDeletedScore) {
    total_hits_.fetch_add(1, std::memory_order_relaxed);
    return true;  // Already deleted
  }

  total_misses_.fetch_add(1, std::memory_order_relaxed);
  return false;  // Not deleted yet
}

void StateCache::UpdateScore(const StateKey& key, int score) {
  std::unique_lock lock(mutex_);

  auto it = states_.find(key);
  if (it != states_.end()) {
    // Update existing entry and move to front
    it->second.first = score;
    TouchLocked(key);
  } else {
    EvictIfFull();
    // Insert new entry at front of LRU
    lru_list_.push_front(key);
    states_.emplace(key, std::make_pair(score, lru_list_.begin()));
  }
}

void StateCache::MarkDeleted(const StateKey& key) {
  std::unique_lock lock(mutex_);

  auto it = states_.find(key);
  if (it != states_.end()) {
    // Update existing entry and move to front
    it->second.first = kDeletedScore;
    TouchLocked(key);
  } else {
    EvictIfFull();
    // Insert new entry at front of LRU
    lru_list_.push_front(key);
    states_.emplace(key, std::make_pair(kDeletedScore, lru_list_.begin()));
  }
}

void StateCache::Clear() {
  std::unique_lock lock(mutex_);
  lru_list_.clear();
  states_.clear();
  total_hits_.store(0, std::memory_order_relaxed);
  total_misses_.store(0, std::memory_order_relaxed);
}

size_t StateCache::Size() const {
  std::shared_lock lock(mutex_);
  return states_.size();
}

StateCache::Statistics StateCache::GetStatistics() const {
  std::shared_lock lock(mutex_);
  Statistics stats;
  stats.size = states_.size();
  stats.max_size = max_size_;
  stats.total_hits = total_hits_.load(std::memory_order_relaxed);
  stats.total_misses = total_misses_.load(std::memory_order_relaxed);
  return stats;
}

void StateCache::EvictIfFull() {
  // LRU eviction: remove least recently used (back of list)
  if (states_.size() >= max_size_ && !lru_list_.empty()) {
    const StateKey& lru_key = lru_list_.back();
    states_.erase(lru_key);
    lru_list_.pop_back();
  }
}

void StateCache::TouchLocked(const StateKey& key) {
  auto it = states_.find(key);
  if (it != states_.end()) {
    lru_list_.erase(it->second.second);
    lru_list_.push_front(key);
    it->second.second = lru_list_.begin();
  }
}

}  // namespace nvecd::events
