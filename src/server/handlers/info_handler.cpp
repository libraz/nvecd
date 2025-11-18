/**
 * @file info_handler.cpp
 * @brief INFO command handler implementation
 */

#include "server/handlers/info_handler.h"

#include <chrono>
#include <sstream>

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

  // Memory section (placeholder)
  oss << "# Memory\n";
  oss << "used_memory_bytes: 0\n";
  oss << "used_memory_human: 0.00 MB\n";
  oss << "memory_health: HEALTHY\n";
  oss << "\n";

  // Data section (placeholder - TODO: add actual counts)
  oss << "# Data\n";
  oss << "id_count: 0\n";
  oss << "ctx_count: 0\n";
  oss << "vector_count: 0\n";
  oss << "event_count: 0\n";

  return oss.str();
}

}  // namespace nvecd::server::handlers
