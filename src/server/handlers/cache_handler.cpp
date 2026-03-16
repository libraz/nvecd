/**
 * @file cache_handler.cpp
 * @brief CACHE command handler implementations
 */

#include "server/handlers/cache_handler.h"

#include <iomanip>
#include <sstream>

#include "cache/similarity_cache.h"

namespace nvecd::server::handlers {

std::string HandleCacheStats(const HandlerContext& ctx) {
  std::ostringstream oss;
  oss << "OK CACHE_STATS\n";

  if (ctx.cache == nullptr) {
    oss << "cache_enabled: false\n";
    oss << "cache_entries: 0\n";
    return oss.str();
  }

  auto stats = ctx.cache->GetStatistics();
  oss << "cache_enabled: " << (ctx.cache->IsEnabled() ? "true" : "false") << "\n";
  oss << "cache_entries: " << stats.current_entries << "\n";
  oss << "cache_memory_bytes: " << stats.current_memory_bytes << "\n";
  oss << "total_queries: " << stats.total_queries << "\n";
  oss << "cache_hits: " << stats.cache_hits << "\n";
  oss << "cache_misses: " << stats.cache_misses << "\n";
  oss << "cache_hit_rate: " << std::fixed << std::setprecision(4) << stats.HitRate() << "\n";
  oss << "evictions: " << stats.evictions << "\n";
  oss << "ttl_expirations: " << stats.ttl_expirations << "\n";
  oss << "total_time_saved_ms: " << std::fixed << std::setprecision(2) << stats.TotalTimeSaved() << "\n";

  return oss.str();
}

std::string HandleCacheClear(HandlerContext& ctx) {
  if (ctx.cache == nullptr) {
    return "OK CACHE_CLEARED (no cache)\n";
  }

  ctx.cache->Clear();
  return "OK CACHE_CLEARED\n";
}

std::string HandleCacheEnable(HandlerContext& ctx) {
  if (ctx.cache == nullptr) {
    return "OK CACHE_ENABLED (no cache instance)\n";
  }

  ctx.cache->SetEnabled(true);
  return "OK CACHE_ENABLED\n";
}

std::string HandleCacheDisable(HandlerContext& ctx) {
  if (ctx.cache == nullptr) {
    return "OK CACHE_DISABLED (no cache instance)\n";
  }

  ctx.cache->SetEnabled(false);
  return "OK CACHE_DISABLED\n";
}

}  // namespace nvecd::server::handlers
