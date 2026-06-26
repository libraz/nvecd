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

/**
 * @brief Build the debug block appended to SIM/SIMV responses in debug mode.
 *
 * Produces a concise "# DEBUG" block with the search mode, query timing, and
 * candidate/result counts. Only appended when the connection has DEBUG mode
 * enabled. Uses CRLF line endings to stay consistent with the rest of the
 * protocol framing.
 *
 * @param mode Search mode label (e.g. "events", "vectors", "fusion", "vector")
 * @param query_time_ms Total query execution time in milliseconds
 * @param candidate_count Number of candidates produced before min_score filtering
 * @param result_count Number of results actually returned to the client
 * @return Formatted debug block terminated with CRLF
 */
std::string FormatSimDebugBlock(const std::string& mode, double query_time_ms, int candidate_count, int result_count);

}  // namespace nvecd::server::handlers
