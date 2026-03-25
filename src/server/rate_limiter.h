/**
 * @file rate_limiter.h
 * @brief Token bucket rate limiter for per-client request throttling
 */

#pragma once

#include <chrono>
#include <list>
#include <mutex>
#include <string>
#include <unordered_map>

namespace nvecd::server {

/**
 * @brief Token bucket rate limiter with per-client tracking
 *
 * Thread-safe. Uses LRU eviction for client tracking.
 */
class RateLimiter {
 public:
  /**
   * @brief Construct rate limiter
   * @param capacity Maximum tokens per client (burst size)
   * @param refill_rate Tokens added per second
   * @param max_clients Maximum tracked clients (LRU eviction)
   */
  RateLimiter(int capacity, int refill_rate, int max_clients);

  /**
   * @brief Check if a request from client_key is allowed
   * @param client_key Client identifier (typically IP address)
   * @return True if request is allowed, false if rate limited
   */
  bool Allow(const std::string& client_key);

 private:
  struct Bucket {
    double tokens;
    std::chrono::steady_clock::time_point last_refill;
  };

  void Refill(Bucket& bucket);
  void EvictOldest();

  const int capacity_;
  const int refill_rate_;
  const int max_clients_;

  std::mutex mutex_;
  std::unordered_map<std::string, Bucket> buckets_;
  std::list<std::string> lru_list_;  ///< Front = most recent
  std::unordered_map<std::string, std::list<std::string>::iterator> lru_map_;
};

}  // namespace nvecd::server
