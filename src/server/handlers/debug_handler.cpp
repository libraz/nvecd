/**
 * @file debug_handler.cpp
 * @brief DEBUG command handler implementation
 */

#include "server/handlers/debug_handler.h"

namespace nvecd::server::handlers {

std::string HandleDebugOn(ConnectionContext& ctx) {
  ctx.debug_mode = true;
  return "OK DEBUG_ON\n";
}

std::string HandleDebugOff(ConnectionContext& ctx) {
  ctx.debug_mode = false;
  return "OK DEBUG_OFF\n";
}

}  // namespace nvecd::server::handlers
