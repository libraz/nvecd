/**
 * @file cache_handler.h
 * @brief CACHE command handlers
 */

#pragma once

#include <string>

#include "server/server_types.h"

namespace nvecd::server::handlers {

/**
 * @brief Handle CACHE STATS command
 * @param ctx Handler context
 * @return Cache statistics response
 */
std::string HandleCacheStats(const HandlerContext& ctx);

/**
 * @brief Handle CACHE CLEAR command
 * @param ctx Handler context
 * @return OK response
 */
std::string HandleCacheClear(HandlerContext& ctx);

/**
 * @brief Handle CACHE ENABLE command
 * @param ctx Handler context
 * @return OK response
 */
std::string HandleCacheEnable(HandlerContext& ctx);

/**
 * @brief Handle CACHE DISABLE command
 * @param ctx Handler context
 * @return OK response
 */
std::string HandleCacheDisable(HandlerContext& ctx);

}  // namespace nvecd::server::handlers
