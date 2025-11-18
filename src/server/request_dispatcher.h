/**
 * @file request_dispatcher.h
 * @brief Request dispatcher for routing commands to handlers
 *
 * Reference: ../mygram-db/src/server/request_dispatcher.h
 * Reusability: 85% (similar pattern, different command types)
 */

#pragma once

#include <string>

#include "server/command_parser.h"
#include "server/server_types.h"
#include "utils/error.h"
#include "utils/expected.h"

namespace nvecd::server {

/**
 * @brief Request dispatcher
 *
 * This class parses commands and routes them to appropriate handlers.
 * It contains pure application logic with no network dependencies.
 *
 * Key responsibilities:
 * - Parse incoming request strings using CommandParser
 * - Validate commands (check server state, etc.)
 * - Route to appropriate handler logic
 * - Format responses
 * - Handle errors gracefully
 *
 * Design principles:
 * - Pure logic, no threading or I/O
 * - Easy to unit test
 * - Clear separation from network layer
 * - Uses Expected<T, Error> for type-safe error handling
 */
class RequestDispatcher {
 public:
  /**
   * @brief Construct a RequestDispatcher
   * @param handler_ctx Handler context (contains core components)
   */
  explicit RequestDispatcher(HandlerContext& handler_ctx);

  // Disable copy and move
  RequestDispatcher(const RequestDispatcher&) = delete;
  RequestDispatcher& operator=(const RequestDispatcher&) = delete;
  RequestDispatcher(RequestDispatcher&&) = delete;
  RequestDispatcher& operator=(RequestDispatcher&&) = delete;

  ~RequestDispatcher() = default;

  /**
   * @brief Dispatch a request to appropriate handler
   * @param request Request string (may be multi-line for VECSET/SIMV)
   * @param conn_ctx Connection context
   * @return Response string
   */
  std::string Dispatch(const std::string& request, ConnectionContext& conn_ctx);

 private:
  // Handler methods
  utils::Expected<std::string, utils::Error> HandleEvent(const Command& cmd);
  utils::Expected<std::string, utils::Error> HandleVecset(const Command& cmd);
  utils::Expected<std::string, utils::Error> HandleSim(const Command& cmd, ConnectionContext& conn_ctx);
  utils::Expected<std::string, utils::Error> HandleSimv(const Command& cmd, ConnectionContext& conn_ctx);
  utils::Expected<std::string, utils::Error> HandleInfo(const Command& cmd);
  utils::Expected<std::string, utils::Error> HandleConfigHelp(const Command& cmd);
  utils::Expected<std::string, utils::Error> HandleConfigShow(const Command& cmd);
  utils::Expected<std::string, utils::Error> HandleConfigVerify(const Command& cmd);
  utils::Expected<std::string, utils::Error> HandleDumpSave(const Command& cmd);
  utils::Expected<std::string, utils::Error> HandleDumpLoad(const Command& cmd);
  utils::Expected<std::string, utils::Error> HandleDumpVerify(const Command& cmd);
  utils::Expected<std::string, utils::Error> HandleDumpInfo(const Command& cmd);
  utils::Expected<std::string, utils::Error> HandleDebugOn(ConnectionContext& conn_ctx);
  utils::Expected<std::string, utils::Error> HandleDebugOff(ConnectionContext& conn_ctx);
  utils::Expected<std::string, utils::Error> HandleCacheStats(const Command& cmd);
  utils::Expected<std::string, utils::Error> HandleCacheClear(const Command& cmd);
  utils::Expected<std::string, utils::Error> HandleCacheEnable(const Command& cmd);
  utils::Expected<std::string, utils::Error> HandleCacheDisable(const Command& cmd);

  // Format response helpers
  std::string FormatOK(const std::string& msg = "") const;
  std::string FormatError(const std::string& msg) const;
  std::string FormatSimResults(const std::vector<std::pair<std::string, float>>& results, int count) const;

  HandlerContext& ctx_;
};

}  // namespace nvecd::server
