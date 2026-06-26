/**
 * @file debug_handler.cpp
 * @brief DEBUG command handler implementation
 */

#include "server/handlers/debug_handler.h"

#include <sstream>

namespace nvecd::server::handlers {

utils::Expected<std::string, utils::Error> HandleDebugOn(ConnectionContext& ctx) {
  ctx.debug_mode = true;
  return std::string("OK Debug mode enabled\r\n");
}

utils::Expected<std::string, utils::Error> HandleDebugOff(ConnectionContext& ctx) {
  ctx.debug_mode = false;
  return std::string("OK Debug mode disabled\r\n");
}

std::string FormatSimDebugBlock(const std::string& mode, double query_time_ms, int candidate_count, int result_count) {
  // Emitted only when the connection has DEBUG mode enabled. The block is
  // appended after the normal SIM/SIMV response and uses CRLF line endings to
  // match the rest of the protocol. Fields mirror the documented debug output:
  // a timing figure and the candidate/result counts for the query.
  const auto query_time_us = static_cast<long long>(query_time_ms * 1000.0);
  std::ostringstream oss;
  oss << "# DEBUG\r\n";
  oss << "mode: " << mode << "\r\n";
  oss << "query_time_us: " << query_time_us << "\r\n";
  oss << "candidates: " << candidate_count << "\r\n";
  oss << "results: " << result_count << "\r\n";
  return oss.str();
}

}  // namespace nvecd::server::handlers
