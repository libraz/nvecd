/**
 * @file debug_handler.cpp
 * @brief DEBUG command handler implementation
 */

#include "server/handlers/debug_handler.h"

namespace nvecd::server::handlers {

utils::Expected<std::string, utils::Error> HandleDebugOn(ConnectionContext& ctx) {
  ctx.debug_mode = true;
  return std::string("OK DEBUG_ON\n");
}

utils::Expected<std::string, utils::Error> HandleDebugOff(ConnectionContext& ctx) {
  ctx.debug_mode = false;
  return std::string("OK DEBUG_OFF\n");
}

}  // namespace nvecd::server::handlers
