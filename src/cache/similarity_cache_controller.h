/**
 * @file similarity_cache_controller.h
 * @brief Synchronized ownership and runtime control for SimilarityCache
 */

#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>

#include "cache/similarity_cache.h"
#include "utils/error.h"
#include "utils/expected.h"

namespace nvecd::cache {

/**
 * Owns the cache for its full server lifetime and atomically publishes it to
 * query handlers only while enabled. Runtime tuning and publication therefore
 * cannot drift between CONFIG, TCP, and HTTP control surfaces.
 */
class SimilarityCacheController {
 public:
  SimilarityCacheController(size_t max_memory_bytes, double min_query_cost_ms, int ttl_seconds,
                            bool compression_enabled, size_t eviction_batch_size, bool enabled,
                            std::atomic<SimilarityCache*>* publication);
  ~SimilarityCacheController() = default;

  SimilarityCacheController(const SimilarityCacheController&) = delete;
  SimilarityCacheController& operator=(const SimilarityCacheController&) = delete;
  SimilarityCacheController(SimilarityCacheController&&) = delete;
  SimilarityCacheController& operator=(SimilarityCacheController&&) = delete;

  utils::Expected<void, utils::Error> SetEnabled(bool enabled);
  utils::Expected<void, utils::Error> SetMinQueryCost(double min_query_cost_ms);
  utils::Expected<void, utils::Error> SetTtl(int ttl_seconds);
  utils::Expected<void, utils::Error> Clear();

  [[nodiscard]] bool IsEnabled() const;
  [[nodiscard]] SimilarityCache& Cache() { return *cache_; }
  [[nodiscard]] const SimilarityCache& Cache() const { return *cache_; }

 private:
  mutable std::mutex mutex_;
  std::unique_ptr<SimilarityCache> cache_;
  std::atomic<SimilarityCache*>* publication_;
  bool enabled_ = false;
};

}  // namespace nvecd::cache
