/**
 * @file admin_handler.cpp
 * @brief Handler for administrative commands
 *
 * Reference: ../mygram-db/src/server/handlers/admin_handler.cpp
 * Reusability: 90% (adapted for nvecd statistics and config)
 * Adapted for: nvecd configuration structure
 */

#include "server/handlers/admin_handler.h"

#include <spdlog/spdlog.h>

#include <sstream>

#include "config/config.h"
#include "config/config_help.h"
#include "utils/structured_log.h"

namespace nvecd::server {

std::string AdminHandler::HandleInfo(const ServerContext& ctx) {
  std::ostringstream oss;

  // Server section
  oss << "# Server\n";
  oss << "version: 0.1.0\n";
  oss << "uptime_seconds: " << ctx.uptime_seconds << "\n";
  oss << "connections_total: " << ctx.connections_total << "\n";
  oss << "connections_current: " << ctx.connections_current << "\n";
  oss << "\n";

  // Vector store section
  oss << "# Vector Store\n";
  oss << "vectors_total: " << ctx.vectors_total << "\n";
  oss << "dimension: " << ctx.vector_dimension << "\n";
  oss << "\n";

  // Event store section
  oss << "# Event Store\n";
  oss << "contexts_total: " << ctx.contexts_total << "\n";
  oss << "events_total: " << ctx.events_total << "\n";
  oss << "\n";

  // Cache section
  oss << "# Cache\n";
  oss << "cache_enabled: " << (ctx.cache_enabled ? "true" : "false") << "\n";
  oss << "cache_hits: " << ctx.cache_hits << "\n";
  oss << "cache_misses: " << ctx.cache_misses << "\n";
  if (ctx.cache_hits + ctx.cache_misses > 0) {
    double hit_rate = static_cast<double>(ctx.cache_hits) / (ctx.cache_hits + ctx.cache_misses) * 100.0;
    oss << "cache_hit_rate: " << std::fixed << std::setprecision(2) << hit_rate << "%\n";
  }
  oss << "\n";

  // Performance section
  oss << "# Performance\n";
  oss << "queries_total: " << ctx.queries_total << "\n";
  oss << "queries_per_second: " << std::fixed << std::setprecision(2) << ctx.queries_per_second << "\n";

  return "+OK\n" + oss.str();
}

std::string AdminHandler::HandleConfigHelp(const std::string& path) {
  try {
    config::ConfigSchemaExplorer explorer;

    if (path.empty()) {
      // Show top-level sections
      auto paths = explorer.ListPaths("");
      std::string result = config::ConfigSchemaExplorer::FormatPathList(paths, "");
      return "+OK\n" + result;
    }

    // Show help for specific path
    auto help_info = explorer.GetHelp(path);
    if (!help_info.has_value()) {
      return "-ERR Configuration path not found: " + path + "\n";
    }

    std::string result = config::ConfigSchemaExplorer::FormatHelp(help_info.value());
    return "+OK\n" + result;

  } catch (const std::exception& e) {
    utils::StructuredLog()
        .Event("server_error")
        .Field("operation", "config_help")
        .Field("error", e.what())
        .Error();
    return "-ERR CONFIG HELP failed: " + std::string(e.what()) + "\n";
  }
}

std::string AdminHandler::HandleConfigShow(const ServerContext& ctx, const std::string& path) {
  try {
    if (ctx.config == nullptr) {
      utils::StructuredLog()
          .Event("server_warning")
          .Field("operation", "config_show")
          .Field("reason", "config_not_available")
          .Warn();
      return "-ERR Server configuration is not available\n";
    }

    std::string result = config::FormatConfigForDisplay(*ctx.config, path);
    return "+OK\n" + result;
  } catch (const std::exception& e) {
    utils::StructuredLog()
        .Event("server_error")
        .Field("operation", "config_show")
        .Field("error", e.what())
        .Error();
    return "-ERR CONFIG SHOW failed: " + std::string(e.what()) + "\n";
  }
}

std::string AdminHandler::HandleConfigVerify(const std::string& filepath) {
  if (filepath.empty()) {
    return "-ERR CONFIG VERIFY requires a filepath\n";
  }

  // Try to load and validate the configuration file
  auto config_result = config::LoadConfig(filepath);
  if (!config_result) {
    utils::StructuredLog()
        .Event("server_error")
        .Field("operation", "config_verify")
        .Field("filepath", filepath)
        .Field("error", config_result.error().to_string())
        .Error();
    return "-ERR Configuration validation failed:\n  " + config_result.error().message() + "\n";
  }

  config::Config test_config = *config_result;

  // Build summary information
  std::ostringstream summary;
  summary << "Configuration is valid\n";
  summary << "  Vectors:\n";
  summary << "    dimension: " << test_config.vectors.default_dimension << "\n";
  summary << "    distance_metric: " << test_config.vectors.distance_metric << "\n";
  summary << "  Events:\n";
  summary << "    ctx_buffer_size: " << test_config.events.ctx_buffer_size << "\n";
  summary << "    decay_interval_sec: " << test_config.events.decay_interval_sec << "\n";
  summary << "  API:\n";
  summary << "    tcp: " << test_config.api.tcp.bind << ":" << test_config.api.tcp.port << "\n";
  if (test_config.api.http.enable) {
    summary << "    http: " << test_config.api.http.bind << ":" << test_config.api.http.port << "\n";
  }

  return "+OK\n" + summary.str();
}

}  // namespace nvecd::server
