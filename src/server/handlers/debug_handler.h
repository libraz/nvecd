/**
 * @file debug_handler.h
 * @brief DEBUG command handler
 *
 * Reference: ../mygram-db/src/server/handlers/debug_handler.h
 * Reusability: 100% (identical logic)
 */

#pragma once

#include <string>

#include "server/server_types.h"
#include "utils/error.h"
#include "utils/expected.h"

namespace nvecd::server::handlers {

/**
 * @brief Handle DEBUG ON command
 *
 * Enables debug mode for the current connection.
 * Debug mode shows detailed execution info for SIM/SIMV commands.
 *
 * @param ctx Connection context
 * @return OK response or error
 */
utils::Expected<std::string, utils::Error> HandleDebugOn(ConnectionContext& ctx);

/**
 * @brief Handle DEBUG OFF command
 *
 * Disables debug mode for the current connection.
 *
 * @param ctx Connection context
 * @return OK response or error
 */
utils::Expected<std::string, utils::Error> HandleDebugOff(ConnectionContext& ctx);

}  // namespace nvecd::server::handlers
