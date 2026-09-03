/**
 * @file admin_handler.cpp
 * @brief Handler for administrative commands
 *
 * Reference: ../mygram-db/src/server/handlers/admin_handler.cpp
 * Reusability: 90% (adapted for nvecd statistics and config)
 * Adapted for: nvecd configuration structure
 */

#include "server/handlers/admin_handler.h"

#include <filesystem>
#include <sstream>
#include <system_error>

#include "config/config.h"
#include "config/config_help.h"
#include "utils/structured_log.h"
#include "version.h"

namespace nvecd::server::handlers {

utils::Expected<std::string, utils::Error> HandleConfigHelp(const std::string& path) {
  auto explorer_result = config::ConfigSchemaExplorer::Create();
  if (!explorer_result) {
    return utils::MakeUnexpected(explorer_result.error());
  }
  auto& explorer = *explorer_result;

  if (path.empty()) {
    // Show top-level sections
    auto paths = explorer.ListPaths("");
    std::string result = config::ConfigSchemaExplorer::FormatPathList(paths, "");
    return std::string("+OK\n") + result + "END\r\n";
  }

  // Show help for specific path
  auto help_info = explorer.GetHelp(path);
  if (!help_info.has_value()) {
    return utils::MakeUnexpected(
        utils::MakeError(utils::ErrorCode::kNotFound, "Configuration path not found: " + path));
  }

  std::string result = config::ConfigSchemaExplorer::FormatHelp(help_info.value());
  return std::string("+OK\n") + result + "END\r\n";
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

  auto result = config::FormatConfigForDisplay(*ctx.config, path);
  if (!result) {
    utils::StructuredLog()
        .Event("server_error")
        .Field("operation", "config_show")
        .Field("error", result.error().message())
        .Error();
    return utils::MakeUnexpected(result.error());
  }
  return std::string("+OK\n") + *result + "END\r\n";
}

namespace {

/// Reported for every path that does not resolve inside the allowed root,
/// whatever the reason. Collapsing "outside the root", "does not exist" and
/// "cannot be opened" into one message keeps the command from answering
/// questions about the filesystem outside the directory an operator opted in.
constexpr const char* kConfigPathNotAllowed = "Configuration file is not accessible";

/**
 * @brief Resolve a client-supplied config path inside the allowed root
 *
 * @param filepath Raw path from the client (absolute, or relative to the root)
 * @param allowed_root Directory the resolved path must reside in
 * @return Canonical path inside @p allowed_root, or a non-identifying error
 */
utils::Expected<std::string, utils::Error> ResolveConfigPath(const std::string& filepath,
                                                             const std::string& allowed_root) {
  const auto denied = [] {
    return utils::MakeUnexpected(utils::MakeError(utils::ErrorCode::kPermissionDenied, kConfigPathNotAllowed));
  };

  if (allowed_root.empty()) {
    return denied();
  }
  // Defense-in-depth: reject traversal segments before canonicalization.
  if (filepath.find("..") != std::string::npos) {
    return denied();
  }

  std::string resolved = filepath;
  if (resolved[0] != '/') {
    resolved = allowed_root + "/" + resolved;
  }

  std::error_code ec;
  const std::filesystem::path root_canonical = std::filesystem::canonical(allowed_root, ec);
  if (ec) {
    return denied();
  }
  const std::filesystem::path resolved_canonical = std::filesystem::canonical(resolved, ec);
  if (ec) {
    return denied();
  }
  const auto relative = resolved_canonical.lexically_relative(root_canonical);
  if (relative.empty() || relative.string().substr(0, 2) == "..") {
    return denied();
  }
  return resolved_canonical.string();
}

}  // namespace

utils::Expected<std::string, utils::Error> HandleConfigVerify(const HandlerContext& ctx, const std::string& filepath) {
  if (filepath.empty()) {
    return utils::MakeUnexpected(
        utils::MakeError(utils::ErrorCode::kInvalidArgument, "CONFIG VERIFY requires a filepath"));
  }

  // A server started without a configuration file has no config directory; the
  // snapshot directory is then the operator-configured root, so the command is
  // never unrooted.
  auto resolved = ResolveConfigPath(filepath, ctx.config_dir.empty() ? ctx.dump_dir : ctx.config_dir);
  if (!resolved) {
    utils::StructuredLog()
        .Event("server_warning")
        .Field("operation", "config_verify")
        .Field("reason", "path_not_allowed")
        .Warn();
    return utils::MakeUnexpected(resolved.error());
  }

  // Try to load and validate the configuration file
  auto config_result = config::LoadConfig(*resolved);
  if (!config_result) {
    utils::StructuredLog()
        .Event("server_error")
        .Field("operation", "config_verify")
        .Field("filepath", *resolved)
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

  return std::string("+OK\n") + summary.str() + "END\r\n";
}

}  // namespace nvecd::server::handlers
