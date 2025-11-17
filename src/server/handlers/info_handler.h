/**
 * @file info_handler.h
 * @brief INFO command handler
 *
 * Reference: ../mygram-db/src/server/handlers/info_handler.h
 * Reusability: 90% (same INFO format, different stats)
 */

#pragma once

#include <string>

#include "server/server_types.h"

namespace nvecd::server::handlers {

/**
 * @brief Handle INFO command
 *
 * Returns Redis-style server statistics:
 * - Server info (version, uptime)
 * - Stats (commands processed, connections)
 * - Memory usage
 * - Data counts (events, vectors, contexts)
 *
 * @param ctx Handler context
 * @return INFO response string
 */
std::string HandleInfo(const HandlerContext& ctx);

}  // namespace nvecd::server::handlers
