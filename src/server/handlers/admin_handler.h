/**
 * @file admin_handler.h
 * @brief Handler for administrative commands (INFO, CONFIG)
 *
 * Reference: ../mygram-db/src/server/handlers/admin_handler.h
 * Reusability: 95% (namespace changes only)
 * Adapted for: nvecd configuration and statistics
 */

#pragma once

#include <string>

#include "server/server_types.h"

namespace nvecd::server {

/**
 * @brief Handler for administrative commands
 *
 * Handles INFO and CONFIG commands for server administration.
 */
class AdminHandler {
 public:
  /**
   * @brief Handle INFO command
   * @param ctx Server context
   * @return Response string
   */
  static std::string HandleInfo(const ServerContext& ctx);

  /**
   * @brief Handle CONFIG HELP command
   * @param path Configuration path (empty for root)
   * @return Response string
   */
  static std::string HandleConfigHelp(const std::string& path);

  /**
   * @brief Handle CONFIG SHOW command
   * @param ctx Server context
   * @param path Configuration path (empty for all)
   * @return Response string
   */
  static std::string HandleConfigShow(const ServerContext& ctx, const std::string& path);

  /**
   * @brief Handle CONFIG VERIFY command
   * @param filepath Path to configuration file
   * @return Response string
   */
  static std::string HandleConfigVerify(const std::string& filepath);
};

}  // namespace nvecd::server
