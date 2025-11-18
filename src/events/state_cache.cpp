/**
 * @file state_cache.cpp
 * @brief State cache implementation
 */

#include "events/state_cache.h"

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
  if (it->second == score) {
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
  if (it->second == kDeletedScore) {
    total_hits_.fetch_add(1, std::memory_order_relaxed);
    return true;  // Already deleted
  }

  total_misses_.fetch_add(1, std::memory_order_relaxed);
  return false;  // Not deleted yet
}

void StateCache::UpdateScore(const StateKey& key, int score) {
  std::unique_lock lock(mutex_);

  EvictIfFull();
  states_[key] = score;
}

void StateCache::MarkDeleted(const StateKey& key) {
  std::unique_lock lock(mutex_);

  EvictIfFull();
  states_[key] = kDeletedScore;
}

void StateCache::Clear() {
  std::unique_lock lock(mutex_);
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
  return Statistics{
      .size = states_.size(),
      .max_size = max_size_,
      .total_hits = total_hits_.load(std::memory_order_relaxed),
      .total_misses = total_misses_.load(std::memory_order_relaxed),
  };
}

void StateCache::EvictIfFull() {
  // Simple eviction: remove first entry if full
  // TODO: Consider LRU eviction for better cache efficiency
  if (states_.size() >= max_size_ && !states_.empty()) {
    states_.erase(states_.begin());
  }
}

}  // namespace nvecd::events
