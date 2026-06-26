/**
 * @file cache_handler.cpp
 * @brief CACHE command handler implementations
 */

#include "server/handlers/cache_handler.h"

#include <iomanip>
#include <sstream>

#include "cache/similarity_cache.h"

namespace nvecd::server::handlers {

utils::Expected<std::string, utils::Error> HandleCacheStats(const HandlerContext& ctx) {
  std::ostringstream oss;
  oss << "OK CACHE_STATS\n";

  auto* cache_ptr = ctx.cache.load(std::memory_order_acquire);
  if (cache_ptr == nullptr) {
    oss << "cache_enabled: false\n";
    oss << "cache_entries: 0\n";
    return oss.str();
  }

  auto stats = cache_ptr->GetStatistics();
  const double current_memory_mb = static_cast<double>(stats.current_memory_bytes) / (1024.0 * 1024.0);
  oss << "cache_enabled: " << (cache_ptr->IsEnabled() ? "true" : "false") << "\n";
  oss << "cache_entries: " << stats.current_entries << "\n";
  oss << "cache_memory_bytes: " << stats.current_memory_bytes << "\n";
  oss << "current_memory_mb: " << std::fixed << std::setprecision(2) << current_memory_mb << "\n";
  oss << "total_queries: " << stats.total_queries << "\n";
  oss << "cache_hits: " << stats.cache_hits << "\n";
  oss << "cache_misses: " << stats.cache_misses << "\n";
  oss << "cache_misses_invalidated: " << stats.cache_misses_invalidated << "\n";
  oss << "cache_misses_not_found: " << stats.cache_misses_not_found << "\n";
  oss << "cache_hit_rate: " << std::fixed << std::setprecision(4) << stats.HitRate() << "\n";
  oss << "evictions: " << stats.evictions << "\n";
  oss << "ttl_expirations: " << stats.ttl_expirations << "\n";
  oss << "avg_hit_latency_ms: " << std::fixed << std::setprecision(3) << stats.AverageCacheHitLatency() << "\n";
  oss << "avg_miss_latency_ms: " << std::fixed << std::setprecision(3) << stats.AverageCacheMissLatency() << "\n";
  oss << "total_time_saved_ms: " << std::fixed << std::setprecision(2) << stats.TotalTimeSaved() << "\n";

  return oss.str();
}

utils::Expected<std::string, utils::Error> HandleCacheClear(HandlerContext& ctx) {
  auto* cache_ptr = ctx.cache.load(std::memory_order_acquire);
  if (cache_ptr == nullptr) {
    return std::string("OK CACHE_CLEARED (no cache)\n");
  }

  cache_ptr->Clear();
  return std::string("OK CACHE_CLEARED\n");
}

utils::Expected<std::string, utils::Error> HandleCacheEnable(HandlerContext& ctx) {
  auto* cache_ptr = ctx.cache.load(std::memory_order_acquire);
  if (cache_ptr == nullptr) {
    return std::string("OK CACHE_ENABLED (no cache instance)\n");
  }

  cache_ptr->SetEnabled(true);
  return std::string("OK CACHE_ENABLED\n");
}

utils::Expected<std::string, utils::Error> HandleCacheDisable(HandlerContext& ctx) {
  auto* cache_ptr = ctx.cache.load(std::memory_order_acquire);
  if (cache_ptr == nullptr) {
    return std::string("OK CACHE_DISABLED (no cache instance)\n");
  }

  cache_ptr->SetEnabled(false);
  return std::string("OK CACHE_DISABLED\n");
}

}  // namespace nvecd::server::handlers
