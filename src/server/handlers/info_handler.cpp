/**
 * @file info_handler.cpp
 * @brief INFO command handler implementation
 *
 * Reference: ../mygram-db/src/server/handlers/admin_handler.cpp
 * Reusability: 90% (adapted for nvecd statistics)
 */

#include "server/handlers/info_handler.h"

#include <iomanip>
#include <sstream>

#include "cache/similarity_cache.h"
#include "events/event_store.h"
#include "vectors/vector_store.h"

namespace nvecd::server::handlers {

std::string HandleInfo(const HandlerContext& ctx) {
  // Reference: ../mygram-db/src/server/handlers/admin_handler.cpp for atomic load pattern
  // All atomic loads are thread-safe without locks

  std::ostringstream oss;

  oss << "OK INFO\n\n";

  // Server section
  oss << "# Server\n";
  oss << "version: 0.1.0\n";
  oss << "uptime_seconds: " << ctx.stats.GetUptimeSeconds() << "\n";
  oss << "\n";

  // Stats section
  oss << "# Stats\n";
  oss << "total_commands_processed: " << ctx.stats.total_commands.load() << "\n";
  oss << "failed_commands: " << ctx.stats.failed_commands.load() << "\n";
  oss << "total_connections_received: " << ctx.stats.total_connections.load() << "\n";
  oss << "active_connections: " << ctx.stats.active_connections.load() << "\n";
  oss << "queries_per_second: " << std::fixed << std::setprecision(2) << ctx.stats.GetQueriesPerSecond() << "\n";
  oss << "\n";

  // Command breakdown
  oss << "# Commands\n";
  oss << "event_commands: " << ctx.stats.event_commands.load() << "\n";
  oss << "sim_commands: " << ctx.stats.sim_commands.load() << "\n";
  oss << "vecset_commands: " << ctx.stats.vecset_commands.load() << "\n";
  oss << "info_commands: " << ctx.stats.info_commands.load() << "\n";
  oss << "config_commands: " << ctx.stats.config_commands.load() << "\n";
  oss << "dump_commands: " << ctx.stats.dump_commands.load() << "\n";
  oss << "cache_commands: " << ctx.stats.cache_commands.load() << "\n";
  oss << "\n";

  // Data section
  oss << "# Data\n";
  if (ctx.vector_store != nullptr) {
    oss << "vector_count: " << ctx.vector_store->GetVectorCount() << "\n";
    oss << "vector_dimension: " << ctx.vector_store->GetDimension() << "\n";
  } else {
    oss << "vector_count: 0\n";
    oss << "vector_dimension: 0\n";
  }

  if (ctx.event_store != nullptr) {
    oss << "ctx_count: " << ctx.event_store->GetContextCount() << "\n";
    oss << "event_count: " << ctx.event_store->GetTotalEventCount() << "\n";
  } else {
    oss << "ctx_count: 0\n";
    oss << "event_count: 0\n";
  }
  oss << "\n";

  // Cache section
  oss << "# Cache\n";
  if (ctx.cache != nullptr) {
    auto cache_stats = ctx.cache->GetStatistics();
    oss << "cache_enabled: true\n";
    oss << "cache_hits: " << cache_stats.cache_hits << "\n";
    oss << "cache_misses: " << cache_stats.cache_misses << "\n";

    uint64_t total_queries = cache_stats.cache_hits + cache_stats.cache_misses;
    if (total_queries > 0) {
      double hit_rate = (static_cast<double>(cache_stats.cache_hits) / static_cast<double>(total_queries)) * 100.0;
      oss << "cache_hit_rate: " << std::fixed << std::setprecision(2) << hit_rate << "%\n";
    } else {
      oss << "cache_hit_rate: 0.00%\n";
    }

    oss << "cache_entries: " << cache_stats.current_entries << "\n";
    oss << "cache_memory_bytes: " << cache_stats.current_memory_bytes << "\n";
    oss << "cache_evictions: " << cache_stats.evictions << "\n";
  } else {
    oss << "cache_enabled: false\n";
  }

  return oss.str();
}

}  // namespace nvecd::server::handlers
