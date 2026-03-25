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
#include "utils/error.h"
#include "utils/expected.h"

namespace nvecd::server::handlers {

/**
 * @brief Handle CONFIG HELP command
 * @param path Configuration path (empty for root)
 * @return Response string or error
 */
utils::Expected<std::string, utils::Error> HandleConfigHelp(const std::string& path);

/**
 * @brief Handle CONFIG SHOW command
 * @param ctx Server context
 * @param path Configuration path (empty for all)
 * @return Response string or error
 */
utils::Expected<std::string, utils::Error> HandleConfigShow(const ServerContext& ctx, const std::string& path);

/**
 * @brief Handle CONFIG VERIFY command
 * @param filepath Path to configuration file
 * @return Response string or error
 */
utils::Expected<std::string, utils::Error> HandleConfigVerify(const std::string& filepath);

}  // namespace nvecd::server::handlers
