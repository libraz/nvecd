/**
 * @file admin_handler.cpp
 * @brief Handler for administrative commands
 *
 * Reference: ../mygram-db/src/server/handlers/admin_handler.cpp
 * Reusability: 90% (adapted for nvecd statistics and config)
 * Adapted for: nvecd configuration structure
 */

#include "server/handlers/admin_handler.h"

#include <sstream>

#include "config/config.h"
#include "config/config_help.h"
#include "utils/structured_log.h"
#include "version.h"

namespace nvecd::server::handlers {

utils::Expected<std::string, utils::Error> HandleConfigHelp(const std::string& path) {
  try {
    config::ConfigSchemaExplorer explorer;

    if (path.empty()) {
      // Show top-level sections
      auto paths = explorer.ListPaths("");
      std::string result = config::ConfigSchemaExplorer::FormatPathList(paths, "");
      return std::string("+OK\n") + result;
    }

    // Show help for specific path
    auto help_info = explorer.GetHelp(path);
    if (!help_info.has_value()) {
      return utils::MakeUnexpected(
          utils::MakeError(utils::ErrorCode::kNotFound, "Configuration path not found: " + path));
    }

    std::string result = config::ConfigSchemaExplorer::FormatHelp(help_info.value());
    return std::string("+OK\n") + result;

  } catch (const std::exception& e) {
    utils::StructuredLog().Event("server_error").Field("operation", "config_help").Field("error", e.what()).Error();
    return utils::MakeUnexpected(
        utils::MakeError(utils::ErrorCode::kInternalError, std::string("CONFIG HELP failed: ") + e.what()));
  }
}

utils::Expected<std::string, utils::Error> HandleConfigShow(const ServerContext& ctx, const std::string& path) {
  if (ctx.config == nullptr) {
    utils::StructuredLog()
        .Event("server_warning")
        .Field("operation", "config_show")
        .Field("reason", "config_not_available")
        .Warn();
    return utils::MakeUnexpected(
        utils::MakeError(utils::ErrorCode::kInternalError, "Server configuration is not available"));
  }

  try {
    std::string result = config::FormatConfigForDisplay(*ctx.config, path);
    return std::string("+OK\n") + result;
  } catch (const std::exception& e) {
    utils::StructuredLog().Event("server_error").Field("operation", "config_show").Field("error", e.what()).Error();
    return utils::MakeUnexpected(
        utils::MakeError(utils::ErrorCode::kInternalError, std::string("CONFIG SHOW failed: ") + e.what()));
  }
}

utils::Expected<std::string, utils::Error> HandleConfigVerify(const std::string& filepath) {
  if (filepath.empty()) {
    return utils::MakeUnexpected(
        utils::MakeError(utils::ErrorCode::kInvalidArgument, "CONFIG VERIFY requires a filepath"));
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
    return utils::MakeUnexpected(
        utils::MakeError(utils::ErrorCode::kConfigValidationError,
                         "Configuration validation failed: " + config_result.error().message()));
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

  return std::string("+OK\n") + summary.str();
}

}  // namespace nvecd::server::handlers
