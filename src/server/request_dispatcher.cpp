/**
 * @file request_dispatcher.cpp
 * @brief Implementation of RequestDispatcher
 *
 * Reference: ../mygram-db/src/server/request_dispatcher.cpp
 * Reusability: 75% (similar dispatch pattern, different handlers)
 */

#include <sstream>

// Include concrete types before request_dispatcher.h to resolve forward declarations
#include "cache/similarity_cache.h"
#include "events/co_occurrence_index.h"
#include "events/event_store.h"
#include "server/handlers/admin_handler.h"
#include "server/handlers/cache_handler.h"
#include "server/handlers/debug_handler.h"
#include "server/handlers/dump_handler.h"
#include "server/handlers/info_handler.h"
#include "server/handlers/variable_handler.h"
#include "server/request_dispatcher.h"
#include "similarity/similarity_engine.h"
#include "utils/error.h"
#include "utils/structured_log.h"
#include "vectors/vector_store.h"

namespace nvecd::server {

RequestDispatcher::RequestDispatcher(HandlerContext& handler_ctx) : ctx_(handler_ctx) {}

std::string RequestDispatcher::Dispatch(const std::string& request, ConnectionContext& conn_ctx) {
  // Parse command
  auto cmd = ParseCommand(request);
  if (!cmd) {
    utils::LogCommandParseError(request, cmd.error().message(), 0);
    return FormatError(cmd.error().message());
  }

  // Route to appropriate handler
  utils::Expected<std::string, utils::Error> result =
      utils::MakeUnexpected(utils::MakeError(utils::ErrorCode::kCommandUnknown, "Unknown command type"));

  switch (cmd->type) {
    case CommandType::kEvent:
      ctx_.stats.event_commands++;
      result = HandleEvent(*cmd);
      break;

    case CommandType::kVecset:
      ctx_.stats.vecset_commands++;
      result = HandleVecset(*cmd);
      break;

    case CommandType::kSim:
      ctx_.stats.sim_commands++;
      result = HandleSim(*cmd, conn_ctx);
      break;

    case CommandType::kSimv:
      ctx_.stats.sim_commands++;
      result = HandleSimv(*cmd, conn_ctx);
      break;

    case CommandType::kInfo:
      ctx_.stats.info_commands++;
      result = HandleInfo(*cmd);
      break;

    case CommandType::kConfigHelp:
      ctx_.stats.config_commands++;
      result = HandleConfigHelp(*cmd);
      break;

    case CommandType::kConfigShow:
      ctx_.stats.config_commands++;
      result = HandleConfigShow(*cmd);
      break;

    case CommandType::kConfigVerify:
      ctx_.stats.config_commands++;
      result = HandleConfigVerify(*cmd);
      break;

    case CommandType::kDumpSave:
      ctx_.stats.dump_commands++;
      result = HandleDumpSave(*cmd);
      break;

    case CommandType::kDumpLoad:
      ctx_.stats.dump_commands++;
      result = HandleDumpLoad(*cmd);
      break;

    case CommandType::kDumpVerify:
      ctx_.stats.dump_commands++;
      result = HandleDumpVerify(*cmd);
      break;

    case CommandType::kDumpInfo:
      ctx_.stats.dump_commands++;
      result = HandleDumpInfo(*cmd);
      break;

    case CommandType::kDebugOn:
      result = HandleDebugOn(conn_ctx);
      break;

    case CommandType::kDebugOff:
      result = HandleDebugOff(conn_ctx);
      break;

    case CommandType::kSet:
      ctx_.stats.config_commands++;
      result = HandleSet(*cmd);
      break;

    case CommandType::kGet:
      ctx_.stats.config_commands++;
      result = HandleGet(*cmd);
      break;

    case CommandType::kShowVariables:
      ctx_.stats.config_commands++;
      result = HandleShowVariables(*cmd);
      break;

    case CommandType::kCacheStats:
      ctx_.stats.cache_commands++;
      result = handlers::HandleCacheStats(ctx_);
      break;

    case CommandType::kCacheClear:
      ctx_.stats.cache_commands++;
      result = handlers::HandleCacheClear(ctx_);
      break;

    case CommandType::kCacheEnable:
      ctx_.stats.cache_commands++;
      result = handlers::HandleCacheEnable(ctx_);
      break;

    case CommandType::kCacheDisable:
      ctx_.stats.cache_commands++;
      result = handlers::HandleCacheDisable(ctx_);
      break;

    case CommandType::kUnknown:
      result = utils::MakeUnexpected(utils::MakeError(utils::ErrorCode::kCommandUnknown, "Unknown command"));
      break;
  }

  // Handle result
  if (!result) {
    ctx_.stats.failed_commands++;
    return FormatError(result.error().message());
  }

  ctx_.stats.total_commands++;
  return *result;
}

//
// Handler implementations
//

utils::Expected<std::string, utils::Error> RequestDispatcher::HandleEvent(const Command& cmd) const {
  if (ctx_.event_store == nullptr) {
    return utils::MakeUnexpected(utils::MakeError(utils::ErrorCode::kInternalError, "EventStore not initialized"));
  }

  auto result = ctx_.event_store->AddEvent(cmd.ctx, cmd.id, cmd.score, cmd.event_type);
  if (!result) {
    return utils::MakeUnexpected(result.error());
  }

  return FormatOK("EVENT");
}

utils::Expected<std::string, utils::Error> RequestDispatcher::HandleVecset(const Command& cmd) const {
  if (ctx_.vector_store == nullptr) {
    return utils::MakeUnexpected(utils::MakeError(utils::ErrorCode::kInternalError, "VectorStore not initialized"));
  }

  auto result = ctx_.vector_store->SetVector(cmd.id, cmd.vector);
  if (!result) {
    return utils::MakeUnexpected(result.error());
  }

  return FormatOK("VECSET");
}

utils::Expected<std::string, utils::Error> RequestDispatcher::HandleSim(const Command& cmd,
                                                                        ConnectionContext& conn_ctx) const {
  (void)conn_ctx;
  if (ctx_.similarity_engine == nullptr) {
    return utils::MakeUnexpected(
        utils::MakeError(utils::ErrorCode::kInternalError, "SimilarityEngine not initialized"));
  }

  // Select search method based on mode
  utils::Expected<std::vector<similarity::SimilarityResult>, utils::Error> result;
  if (cmd.mode == "events") {
    result = ctx_.similarity_engine->SearchByIdEvents(cmd.id, cmd.top_k);
  } else if (cmd.mode == "vectors") {
    result = ctx_.similarity_engine->SearchByIdVectors(cmd.id, cmd.top_k);
  } else {  // fusion (default)
    result = ctx_.similarity_engine->SearchByIdFusion(cmd.id, cmd.top_k);
  }

  if (!result) {
    return utils::MakeUnexpected(result.error());
  }

  // Convert SimilarityResult to pair<string, float>
  std::vector<std::pair<std::string, float>> pairs;
  pairs.reserve(result->size());
  for (const auto& item : *result) {
    pairs.emplace_back(item.item_id, item.score);
  }

  return FormatSimResults(pairs, static_cast<int>(pairs.size()));
}

utils::Expected<std::string, utils::Error> RequestDispatcher::HandleSimv(const Command& cmd,
                                                                         ConnectionContext& conn_ctx) const {
  (void)conn_ctx;
  if (ctx_.similarity_engine == nullptr) {
    return utils::MakeUnexpected(
        utils::MakeError(utils::ErrorCode::kInternalError, "SimilarityEngine not initialized"));
  }

  auto result = ctx_.similarity_engine->SearchByVector(cmd.vector, cmd.top_k);
  if (!result) {
    return utils::MakeUnexpected(result.error());
  }

  // Convert SimilarityResult to pair<string, float>
  std::vector<std::pair<std::string, float>> pairs;
  pairs.reserve(result->size());
  for (const auto& item : *result) {
    pairs.emplace_back(item.item_id, item.score);
  }

  return FormatSimResults(pairs, static_cast<int>(pairs.size()));
}

utils::Expected<std::string, utils::Error> RequestDispatcher::HandleInfo(const Command& /* cmd */) {
  return handlers::HandleInfo(ctx_);
}

utils::Expected<std::string, utils::Error> RequestDispatcher::HandleConfigHelp(const Command& cmd) {
  return handlers::HandleConfigHelp(cmd.path);
}

utils::Expected<std::string, utils::Error> RequestDispatcher::HandleConfigShow(const Command& cmd) {
  // Build ServerContext from HandlerContext members
  ServerContext server_ctx;
  server_ctx.config = ctx_.config;
  server_ctx.uptime_seconds = ctx_.stats.GetUptimeSeconds();
  server_ctx.connections_total = ctx_.stats.total_connections.load();
  server_ctx.connections_current = ctx_.stats.active_connections.load();
  server_ctx.vectors_total = ctx_.vector_store != nullptr ? ctx_.vector_store->GetVectorCount() : 0;
  server_ctx.vector_dimension =
      ctx_.vector_store != nullptr ? static_cast<uint32_t>(ctx_.vector_store->GetDimension()) : 0;
  server_ctx.contexts_total = ctx_.event_store != nullptr ? ctx_.event_store->GetContextCount() : 0;
  server_ctx.events_total = ctx_.event_store != nullptr ? ctx_.event_store->GetTotalEventCount() : 0;
  auto* cache_ptr = ctx_.cache.load(std::memory_order_acquire);
  server_ctx.cache_enabled = (cache_ptr != nullptr);
  if (cache_ptr != nullptr) {
    auto cache_stats = cache_ptr->GetStatistics();
    server_ctx.cache_hits = cache_stats.cache_hits;
    server_ctx.cache_misses = cache_stats.cache_misses;
  }
  server_ctx.queries_total = ctx_.stats.total_commands.load();
  server_ctx.queries_per_second = ctx_.stats.GetQueriesPerSecond();

  return handlers::HandleConfigShow(server_ctx, cmd.path);
}

utils::Expected<std::string, utils::Error> RequestDispatcher::HandleConfigVerify(const Command& cmd) {
  return handlers::HandleConfigVerify(cmd.path);
}

utils::Expected<std::string, utils::Error> RequestDispatcher::HandleDumpSave(const Command& cmd) {
  return handlers::HandleDumpSave(ctx_, cmd.path);
}

utils::Expected<std::string, utils::Error> RequestDispatcher::HandleDumpLoad(const Command& cmd) {
  return handlers::HandleDumpLoad(ctx_, cmd.path);
}

utils::Expected<std::string, utils::Error> RequestDispatcher::HandleDumpVerify(const Command& cmd) const {
  return handlers::HandleDumpVerify(ctx_.dump_dir, cmd.path);
}

utils::Expected<std::string, utils::Error> RequestDispatcher::HandleDumpInfo(const Command& cmd) const {
  return handlers::HandleDumpInfo(ctx_.dump_dir, cmd.path);
}

utils::Expected<std::string, utils::Error> RequestDispatcher::HandleDebugOn(ConnectionContext& conn_ctx) {
  return handlers::HandleDebugOn(conn_ctx);
}

utils::Expected<std::string, utils::Error> RequestDispatcher::HandleDebugOff(ConnectionContext& conn_ctx) {
  return handlers::HandleDebugOff(conn_ctx);
}

//
// Format helpers
//

std::string RequestDispatcher::FormatOK(const std::string& msg) {
  if (msg.empty()) {
    return "OK\r\n";
  }
  return "OK " + msg + "\r\n";
}

std::string RequestDispatcher::FormatError(const std::string& msg) {
  return "ERROR " + msg + "\r\n";
}

std::string RequestDispatcher::FormatSimResults(const std::vector<std::pair<std::string, float>>& results, int count) {
  std::ostringstream oss;
  oss << "OK RESULTS " << count << "\r\n";
  for (const auto& [id, score] : results) {
    oss << id << " " << score << "\r\n";
  }
  return oss.str();
}

//
// Variable command handlers
//

utils::Expected<std::string, utils::Error> RequestDispatcher::HandleSet(const Command& cmd) {
  return handlers::HandleSet(ctx_.variable_manager, cmd.variable_name, cmd.variable_value);
}

utils::Expected<std::string, utils::Error> RequestDispatcher::HandleGet(const Command& cmd) {
  return handlers::HandleGet(ctx_.variable_manager, cmd.variable_name);
}

utils::Expected<std::string, utils::Error> RequestDispatcher::HandleShowVariables(const Command& cmd) {
  return handlers::HandleShowVariables(ctx_.variable_manager, cmd.pattern);
}

}  // namespace nvecd::server
