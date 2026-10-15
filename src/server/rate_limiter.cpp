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

  auto it = clients_.find(client_key);
  if (it == clients_.end()) {
    // New client - create bucket with full tokens
    if (static_cast<int>(clients_.size()) >= max_clients_) {
      EvictOldest();
    }

    lru_list_.push_front(client_key);
    Bucket bucket{static_cast<double>(capacity_) - 1.0, std::chrono::steady_clock::now()};
    clients_.emplace(client_key, Client{bucket, lru_list_.begin()});
    return true;
  }

  // Existing client - refill and check
  Refill(it->second.bucket);
  lru_list_.splice(lru_list_.begin(), lru_list_, it->second.lru_position);

  if (it->second.bucket.tokens >= 1.0) {
    it->second.bucket.tokens -= 1.0;
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
  clients_.erase(lru_list_.back());
  lru_list_.pop_back();
}

}  // namespace nvecd::server
