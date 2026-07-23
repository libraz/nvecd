/**
 * @file cache_handler.cpp
 * @brief CACHE command handler implementations
 */

#include "server/handlers/cache_handler.h"

#include <iomanip>
#include <sstream>

#include "cache/similarity_cache_controller.h"
#include "config/runtime_variable_manager.h"

namespace nvecd::server::handlers {

utils::Expected<std::string, utils::Error> HandleCacheStats(const HandlerContext& ctx) {
  std::ostringstream oss;
  oss << "OK CACHE_STATS\n";

  if (ctx.cache_controller == nullptr) {
    return utils::MakeUnexpected(
        utils::MakeError(utils::ErrorCode::kInternalError, "Cache controller is not initialized"));
  }

  auto stats = ctx.cache_controller->Cache().GetStatistics();
  const double current_memory_mb = static_cast<double>(stats.current_memory_bytes) / (1024.0 * 1024.0);
  oss << "cache_enabled: " << (ctx.cache_controller->IsEnabled() ? "true" : "false") << "\n";
  oss << "cache_entries: " << stats.current_entries << "\n";
  oss << "cache_memory_bytes: " << stats.current_memory_bytes << "\n";
  oss << "current_memory_mb: " << std::fixed << std::setprecision(2) << current_memory_mb << "\n";
  oss << "min_query_cost_ms: " << ctx.cache_controller->Cache().GetMinQueryCost() << "\n";
  oss << "ttl_seconds: " << ctx.cache_controller->Cache().GetTtl() << "\n";
  oss << "compression_enabled: " << (ctx.cache_controller->Cache().CompressionEnabled() ? "true" : "false") << "\n";
  oss << "eviction_batch_size: " << ctx.cache_controller->Cache().EvictionBatchSize() << "\n";
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
  oss << "END\r\n";

  return oss.str();
}

utils::Expected<std::string, utils::Error> HandleCacheClear(HandlerContext& ctx) {
  if (ctx.cache_controller == nullptr) {
    return utils::MakeUnexpected(
        utils::MakeError(utils::ErrorCode::kInternalError, "Cache controller is not initialized"));
  }
  auto result = ctx.cache_controller->Clear();
  if (!result) {
    return utils::MakeUnexpected(result.error());
  }
  return std::string("OK CACHE_CLEARED\n");
}

utils::Expected<std::string, utils::Error> HandleCacheEnable(HandlerContext& ctx) {
  if (ctx.variable_manager == nullptr) {
    return utils::MakeUnexpected(
        utils::MakeError(utils::ErrorCode::kInternalError, "Runtime variable manager is not initialized"));
  }
  auto result = ctx.variable_manager->SetVariable("cache.enabled", "true");
  if (!result) {
    return utils::MakeUnexpected(result.error());
  }
  return std::string("OK CACHE_ENABLED\n");
}

utils::Expected<std::string, utils::Error> HandleCacheDisable(HandlerContext& ctx) {
  if (ctx.variable_manager == nullptr) {
    return utils::MakeUnexpected(
        utils::MakeError(utils::ErrorCode::kInternalError, "Runtime variable manager is not initialized"));
  }
  auto result = ctx.variable_manager->SetVariable("cache.enabled", "false");
  if (!result) {
    return utils::MakeUnexpected(result.error());
  }
  return std::string("OK CACHE_DISABLED\n");
}

}  // namespace nvecd::server::handlers
