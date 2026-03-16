/**
 * @file info_handler.cpp
 * @brief INFO command handler implementation
 */

#include "server/handlers/info_handler.h"

#include <chrono>
#include <iomanip>
#include <sstream>

#include "cache/similarity_cache.h"
#include "events/co_occurrence_index.h"
#include "events/event_store.h"
#include "vectors/vector_store.h"

namespace nvecd::server::handlers {

namespace {

// Server start time (const because it's set once at startup)
const auto g_server_start_time = std::chrono::steady_clock::now();

}  // namespace

std::string HandleInfo(const HandlerContext& ctx) {
  std::ostringstream oss;

  // Calculate uptime
  auto now = std::chrono::steady_clock::now();
  auto uptime_sec = std::chrono::duration_cast<std::chrono::seconds>(now - g_server_start_time).count();

  oss << "OK INFO\n\n";

  // Server section
  oss << "# Server\n";
  oss << "version: 0.1.0\n";
  oss << "uptime_seconds: " << uptime_sec << "\n";
  oss << "\n";

  // Stats section
  oss << "# Stats\n";
  oss << "total_commands_processed: " << ctx.stats.total_commands.load() << "\n";
  oss << "failed_commands: " << ctx.stats.failed_commands.load() << "\n";
  oss << "total_connections_received: " << ctx.stats.total_connections.load() << "\n";
  oss << "active_connections: " << ctx.stats.active_connections.load() << "\n";
  oss << "event_commands: " << ctx.stats.event_commands.load() << "\n";
  oss << "sim_commands: " << ctx.stats.sim_commands.load() << "\n";
  oss << "vecset_commands: " << ctx.stats.vecset_commands.load() << "\n";
  oss << "\n";

  // Memory section
  size_t dimension = ctx.vector_store != nullptr ? ctx.vector_store->GetDimension() : 0;
  uint64_t vec_count = ctx.vector_store != nullptr ? ctx.vector_store->GetVectorCount() : 0;
  uint64_t used_memory_bytes = vec_count * dimension * sizeof(float);
  double used_memory_mb = static_cast<double>(used_memory_bytes) / (1024.0 * 1024.0);
  oss << "# Memory\n";
  oss << "used_memory_bytes: " << used_memory_bytes << "\n";
  oss << "used_memory_human: " << std::fixed << std::setprecision(2) << used_memory_mb << " MB\n";
  oss << "memory_health: HEALTHY\n";

  // Cache section
  oss << "\n";
  oss << "# Cache\n";
  if (ctx.cache != nullptr) {
    auto cache_stats = ctx.cache->GetStatistics();
    oss << "cache_entries: " << cache_stats.current_entries << "\n";
    oss << "cache_hits: " << cache_stats.cache_hits << "\n";
    oss << "cache_misses: " << cache_stats.cache_misses << "\n";
    oss << "cache_hit_rate: " << std::fixed << std::setprecision(4) << cache_stats.HitRate() << "\n";
  } else {
    oss << "cache_entries: 0\n";
    oss << "cache_hits: 0\n";
    oss << "cache_misses: 0\n";
    oss << "cache_hit_rate: 0.0000\n";
  }
  oss << "\n";

  // Data section
  oss << "# Data\n";
  uint64_t id_count = ctx.co_index != nullptr ? ctx.co_index->GetItemCount() : 0;
  uint64_t ctx_count = ctx.event_store != nullptr ? ctx.event_store->GetContextCount() : 0;
  uint64_t vector_count = ctx.vector_store != nullptr ? ctx.vector_store->GetVectorCount() : 0;
  uint64_t event_count = ctx.event_store != nullptr ? ctx.event_store->GetTotalEventCount() : 0;
  oss << "id_count: " << id_count << "\n";
  oss << "ctx_count: " << ctx_count << "\n";
  oss << "vector_count: " << vector_count << "\n";
  oss << "event_count: " << event_count << "\n";

  return oss.str();
}

}  // namespace nvecd::server::handlers
