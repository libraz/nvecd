/**
 * @file rate_limiter.cpp
 * @brief Token bucket rate limiter implementation
 */

#include "server/rate_limiter.h"

#include <algorithm>

namespace nvecd::server {

RateLimiter::RateLimiter(int capacity, int refill_rate, int max_clients)
    : capacity_(capacity), refill_rate_(refill_rate), max_clients_(max_clients) {}

bool RateLimiter::Allow(const std::string& client_key) {
  std::lock_guard<std::mutex> lock(mutex_);

  auto it = buckets_.find(client_key);
  if (it == buckets_.end()) {
    // New client - create bucket with full tokens
    if (static_cast<int>(buckets_.size()) >= max_clients_) {
      EvictOldest();
    }

    Bucket bucket;
    bucket.tokens = static_cast<double>(capacity_) - 1.0;  // Consume 1 for this request
    bucket.last_refill = std::chrono::steady_clock::now();
    buckets_[client_key] = bucket;

    lru_list_.push_front(client_key);
    lru_map_[client_key] = lru_list_.begin();
    return true;
  }

  // Existing client - refill and check
  Refill(it->second);

  // Update LRU
  auto lru_it = lru_map_.find(client_key);
  if (lru_it != lru_map_.end()) {
    lru_list_.erase(lru_it->second);
    lru_list_.push_front(client_key);
    lru_map_[client_key] = lru_list_.begin();
  }

  if (it->second.tokens >= 1.0) {
    it->second.tokens -= 1.0;
    return true;
  }

  return false;  // Rate limited
}

void RateLimiter::Refill(Bucket& bucket) {
  auto now = std::chrono::steady_clock::now();
  double elapsed_sec = std::chrono::duration<double>(now - bucket.last_refill).count();
  bucket.tokens = std::min(static_cast<double>(capacity_), bucket.tokens + elapsed_sec * refill_rate_);
  bucket.last_refill = now;
}

void RateLimiter::EvictOldest() {
  if (lru_list_.empty()) {
    return;
  }
  const auto& oldest = lru_list_.back();
  buckets_.erase(oldest);
  lru_map_.erase(oldest);
  lru_list_.pop_back();
}

}  // namespace nvecd::server
