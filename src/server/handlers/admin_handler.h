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
 *
 * The path comes from a network client, so it is canonicalised and confined to
 * an allowed root the same way the sibling DUMP commands are confined to
 * HandlerContext::dump_dir. A path that resolves outside the root is answered
 * with one non-identifying error, so the command cannot be used to probe the
 * existence, readability or parseability of arbitrary files.
 *
 * @param ctx Handler context supplying the allowed root
 * @param filepath Path to configuration file
 * @return Response string or error
 */
utils::Expected<std::string, utils::Error> HandleConfigVerify(const HandlerContext& ctx, const std::string& filepath);

}  // namespace nvecd::server::handlers
