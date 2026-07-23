/**
 * @file similarity_cache_controller.cpp
 * @brief SimilarityCacheController implementation
 */

#include "cache/similarity_cache_controller.h"

#include <cmath>

namespace nvecd::cache {

SimilarityCacheController::SimilarityCacheController(size_t max_memory_bytes, double min_query_cost_ms, int ttl_seconds,
                                                     bool compression_enabled, size_t eviction_batch_size, bool enabled,
                                                     std::atomic<SimilarityCache*>* publication)
    : cache_(std::make_unique<SimilarityCache>(max_memory_bytes, min_query_cost_ms, ttl_seconds, compression_enabled,
                                               eviction_batch_size)),
      publication_(publication),
      enabled_(enabled) {
  cache_->SetEnabled(enabled_);
  if (publication_ != nullptr) {
    publication_->store(enabled_ ? cache_.get() : nullptr, std::memory_order_release);
  }
}

utils::Expected<void, utils::Error> SimilarityCacheController::SetEnabled(bool enabled) {
  std::scoped_lock lock(mutex_);
  enabled_ = enabled;
  cache_->SetEnabled(enabled);
  if (publication_ != nullptr) {
    publication_->store(enabled ? cache_.get() : nullptr, std::memory_order_release);
  }
  return {};
}

utils::Expected<void, utils::Error> SimilarityCacheController::SetMinQueryCost(double min_query_cost_ms) {
  if (!std::isfinite(min_query_cost_ms) || min_query_cost_ms < 0.0) {
    return utils::MakeUnexpected(
        utils::MakeError(utils::ErrorCode::kInvalidArgument, "cache.min_query_cost_ms must be finite and >= 0"));
  }
  std::scoped_lock lock(mutex_);
  cache_->SetMinQueryCost(min_query_cost_ms);
  return {};
}

utils::Expected<void, utils::Error> SimilarityCacheController::SetTtl(int ttl_seconds) {
  if (ttl_seconds < 0) {
    return utils::MakeUnexpected(
        utils::MakeError(utils::ErrorCode::kInvalidArgument, "cache.ttl_seconds must be >= 0"));
  }
  std::scoped_lock lock(mutex_);
  cache_->SetTtl(ttl_seconds);
  return {};
}

utils::Expected<void, utils::Error> SimilarityCacheController::Clear() {
  std::scoped_lock lock(mutex_);
  cache_->Clear();
  return {};
}

bool SimilarityCacheController::IsEnabled() const {
  std::scoped_lock lock(mutex_);
  return enabled_;
}

}  // namespace nvecd::cache
