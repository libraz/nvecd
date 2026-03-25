/**
 * @file cache_handler.h
 * @brief CACHE command handlers
 */

#pragma once

#include <string>

#include "server/server_types.h"
#include "utils/error.h"
#include "utils/expected.h"

namespace nvecd::server::handlers {

/**
 * @brief Handle CACHE STATS command
 * @param ctx Handler context
 * @return Cache statistics response or error
 */
utils::Expected<std::string, utils::Error> HandleCacheStats(const HandlerContext& ctx);

/**
 * @brief Handle CACHE CLEAR command
 * @param ctx Handler context
 * @return OK response or error
 */
utils::Expected<std::string, utils::Error> HandleCacheClear(HandlerContext& ctx);

/**
 * @brief Handle CACHE ENABLE command
 * @param ctx Handler context
 * @return OK response or error
 */
utils::Expected<std::string, utils::Error> HandleCacheEnable(HandlerContext& ctx);

/**
 * @brief Handle CACHE DISABLE command
 * @param ctx Handler context
 * @return OK response or error
 */
utils::Expected<std::string, utils::Error> HandleCacheDisable(HandlerContext& ctx);

}  // namespace nvecd::server::handlers
