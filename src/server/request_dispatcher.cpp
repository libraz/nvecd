/**
 * @file request_dispatcher.cpp
 * @brief Implementation of RequestDispatcher
 *
 * Reference: ../mygram-db/src/server/request_dispatcher.cpp
 * Reusability: 75% (similar dispatch pattern, different handlers)
 */

#include <sstream>
#include <array>
#include <chrono>
#include <ctime>
#include <filesystem>

// Include concrete types before request_dispatcher.h to resolve forward declarations
#include "cache/similarity_cache.h"
#include "events/co_occurrence_index.h"
#include "events/event_store.h"
#include "similarity/similarity_engine.h"
#include "vectors/vector_store.h"

#include "server/request_dispatcher.h"
#include "cache/cache_key_generator.h"
#include "server/handlers/admin_handler.h"
#include "server/handlers/debug_handler.h"
#include "server/handlers/info_handler.h"
#include "storage/snapshot_format_v1.h"
#include "utils/error.h"
#include "utils/structured_log.h"

namespace nvecd::server {

namespace {
constexpr int kFilepathBufferSize = 256;

/**
 * @brief RAII guard for atomic boolean flags
 *
 * Reference: ../mygram-db/src/server/handlers/dump_handler.cpp
 * Reusability: 100%
 *
 * Automatically sets flag to true on construction and resets to false on destruction.
 * Exception-safe: ensures flag is always reset even if exceptions are thrown.
 */
class FlagGuard {
 public:
  explicit FlagGuard(std::atomic<bool>& flag) : flag_(flag) { flag_ = true; }
  ~FlagGuard() { flag_ = false; }

  // Non-copyable and non-movable
  FlagGuard(const FlagGuard&) = delete;
  FlagGuard& operator=(const FlagGuard&) = delete;
  FlagGuard(FlagGuard&&) = delete;
  FlagGuard& operator=(FlagGuard&&) = delete;

 private:
  std::atomic<bool>& flag_;
};

}  // namespace

RequestDispatcher::RequestDispatcher(HandlerContext& handler_ctx) : ctx_(handler_ctx) {}

std::string RequestDispatcher::Dispatch(const std::string& request, ConnectionContext& conn_ctx) {
  // Parse command
  auto cmd = ParseCommand(request);
  if (!cmd) {
    utils::LogCommandParseError(request, cmd.error().message(), 0);
    return FormatError(cmd.error().message());
  }

  // Route to appropriate handler
  utils::Expected<std::string, utils::Error> result = utils::MakeUnexpected(
      utils::MakeError(utils::ErrorCode::kCommandUnknown, "Unknown command type"));

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
    case CommandType::kConfigShow:
    case CommandType::kConfigVerify:
      ctx_.stats.config_commands++;
      if (cmd->type == CommandType::kConfigHelp) {
        result = HandleConfigHelp(*cmd);
      } else if (cmd->type == CommandType::kConfigShow) {
        result = HandleConfigShow(*cmd);
      } else {
        result = HandleConfigVerify(*cmd);
      }
      break;

    case CommandType::kDumpSave:
    case CommandType::kDumpLoad:
    case CommandType::kDumpVerify:
    case CommandType::kDumpInfo:
      ctx_.stats.dump_commands++;
      if (cmd->type == CommandType::kDumpSave) {
        result = HandleDumpSave(*cmd);
      } else if (cmd->type == CommandType::kDumpLoad) {
        result = HandleDumpLoad(*cmd);
      } else if (cmd->type == CommandType::kDumpVerify) {
        result = HandleDumpVerify(*cmd);
      } else {
        result = HandleDumpInfo(*cmd);
      }
      break;

    case CommandType::kDebugOn:
      result = HandleDebugOn(conn_ctx);
      break;

    case CommandType::kDebugOff:
      result = HandleDebugOff(conn_ctx);
      break;

    case CommandType::kCacheStats:
    case CommandType::kCacheClear:
    case CommandType::kCacheEnable:
    case CommandType::kCacheDisable:
      ctx_.stats.cache_commands++;
      if (cmd->type == CommandType::kCacheStats) {
        result = HandleCacheStats(*cmd);
      } else if (cmd->type == CommandType::kCacheClear) {
        result = HandleCacheClear(*cmd);
      } else if (cmd->type == CommandType::kCacheEnable) {
        result = HandleCacheEnable(*cmd);
      } else {
        result = HandleCacheDisable(*cmd);
      }
      break;

    case CommandType::kUnknown:
      result = utils::MakeUnexpected(
          utils::MakeError(utils::ErrorCode::kCommandUnknown, "Unknown command"));
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

utils::Expected<std::string, utils::Error> RequestDispatcher::HandleEvent(const Command& cmd) {
  if (ctx_.event_store == nullptr) {
    return utils::MakeUnexpected(utils::MakeError(utils::ErrorCode::kInternalError, "EventStore not initialized"));
  }

  auto result = ctx_.event_store->AddEvent(cmd.ctx, cmd.id, cmd.score);
  if (!result) {
    return utils::MakeUnexpected(result.error());
  }

  // Invalidate fusion mode cache entries for this ID (if cache enabled)
  // Events affect co-occurrence scores, which affect fusion search results
  if (ctx_.cache != nullptr) {
    const std::vector<int> common_top_k = {10, 20, 50, 100};
    for (int top_k : common_top_k) {
      auto key = cache::GenerateSimCacheKey(cmd.id, top_k, "fusion");
      ctx_.cache->Erase(key);
    }
  }

  return FormatOK("EVENT");
}

utils::Expected<std::string, utils::Error> RequestDispatcher::HandleVecset(const Command& cmd) {
  if (ctx_.vector_store == nullptr) {
    return utils::MakeUnexpected(utils::MakeError(utils::ErrorCode::kInternalError, "VectorStore not initialized"));
  }

  auto result = ctx_.vector_store->SetVector(cmd.id, cmd.vector);
  if (!result) {
    return utils::MakeUnexpected(result.error());
  }

  // Invalidate cache entries for this ID (if cache enabled)
  if (ctx_.cache != nullptr) {
    // Simple approach: invalidate all SIM cache entries for this specific ID
    // We generate keys for common top_k values and modes
    const std::vector<std::string> modes = {"vectors", "events", "fusion"};
    const std::vector<int> common_top_k = {10, 20, 50, 100};  // Common top_k values

    for (const auto& mode : modes) {
      for (int top_k : common_top_k) {
        auto key = cache::GenerateSimCacheKey(cmd.id, top_k, mode);
        ctx_.cache->Erase(key);
      }
    }

    // TODO: Implement reverse index (ID -> cache keys) for more efficient invalidation
    // Alternative: Clear all cache (simple but may impact performance)
    // ctx_.cache->Clear();
  }

  return FormatOK("VECSET");
}

utils::Expected<std::string, utils::Error> RequestDispatcher::HandleSim(const Command& cmd,
                                                                         ConnectionContext& conn_ctx) {
  (void)conn_ctx;  // TODO: Use for debug mode
  if (ctx_.similarity_engine == nullptr) {
    return utils::MakeUnexpected(
        utils::MakeError(utils::ErrorCode::kInternalError, "SimilarityEngine not initialized"));
  }

  // Generate cache key
  auto cache_key = cache::GenerateSimCacheKey(cmd.id, cmd.top_k, cmd.mode);

  // Try cache lookup (if cache enabled)
  if (ctx_.cache != nullptr) {
    auto cached = ctx_.cache->Lookup(cache_key);
    if (cached) {
      // Cache hit! Return cached results
      std::vector<std::pair<std::string, float>> pairs;
      pairs.reserve(cached->size());
      for (const auto& r : *cached) {
        pairs.emplace_back(r.id, r.score);
      }
      return FormatSimResults(pairs, static_cast<int>(pairs.size()));
    }
  }

  // Cache miss or cache disabled - execute query
  auto start_query = std::chrono::high_resolution_clock::now();

  // Select search method based on mode
  utils::Expected<std::vector<similarity::SimilarityResult>, utils::Error> result;
  if (cmd.mode == "events") {
    result = ctx_.similarity_engine->SearchByIdEvents(cmd.id, cmd.top_k);
  } else if (cmd.mode == "vectors") {
    result = ctx_.similarity_engine->SearchByIdVectors(cmd.id, cmd.top_k);
  } else {  // fusion (default)
    result = ctx_.similarity_engine->SearchByIdFusion(cmd.id, cmd.top_k);
  }

  auto end_query = std::chrono::high_resolution_clock::now();
  double query_cost_ms = std::chrono::duration<double, std::milli>(end_query - start_query).count();

  if (!result) {
    return utils::MakeUnexpected(result.error());
  }

  // Insert into cache (if enabled and successful)
  if (ctx_.cache != nullptr) {
    ctx_.cache->Insert(cache_key, *result, query_cost_ms);
  }

  // Convert SimilarityResult to pair<string, float>
  std::vector<std::pair<std::string, float>> pairs;
  pairs.reserve(result->size());
  for (const auto& r : *result) {
    pairs.emplace_back(r.id, r.score);
  }

  return FormatSimResults(pairs, static_cast<int>(pairs.size()));
}

utils::Expected<std::string, utils::Error> RequestDispatcher::HandleSimv(const Command& cmd,
                                                                          ConnectionContext& conn_ctx) {
  (void)conn_ctx;  // TODO: Use for debug mode
  if (ctx_.similarity_engine == nullptr) {
    return utils::MakeUnexpected(
        utils::MakeError(utils::ErrorCode::kInternalError, "SimilarityEngine not initialized"));
  }

  // Generate cache key (SIMV queries use "vectors" mode implicitly)
  auto cache_key = cache::GenerateSimvCacheKey(cmd.vector, cmd.top_k, "vectors");

  // Try cache lookup (if cache enabled)
  if (ctx_.cache != nullptr) {
    auto cached = ctx_.cache->Lookup(cache_key);
    if (cached) {
      // Cache hit! Return cached results
      std::vector<std::pair<std::string, float>> pairs;
      pairs.reserve(cached->size());
      for (const auto& r : *cached) {
        pairs.emplace_back(r.id, r.score);
      }
      return FormatSimResults(pairs, static_cast<int>(pairs.size()));
    }
  }

  // Cache miss or cache disabled - execute query
  auto start_query = std::chrono::high_resolution_clock::now();

  auto result = ctx_.similarity_engine->SearchByVector(cmd.vector, cmd.top_k);

  auto end_query = std::chrono::high_resolution_clock::now();
  double query_cost_ms = std::chrono::duration<double, std::milli>(end_query - start_query).count();

  if (!result) {
    return utils::MakeUnexpected(result.error());
  }

  // Insert into cache (if enabled and successful)
  if (ctx_.cache != nullptr) {
    ctx_.cache->Insert(cache_key, *result, query_cost_ms);
  }

  // Convert SimilarityResult to pair<string, float>
  std::vector<std::pair<std::string, float>> pairs;
  pairs.reserve(result->size());
  for (const auto& r : *result) {
    pairs.emplace_back(r.id, r.score);
  }

  return FormatSimResults(pairs, static_cast<int>(pairs.size()));
}

utils::Expected<std::string, utils::Error> RequestDispatcher::HandleInfo(const Command& /* cmd */) {
  return handlers::HandleInfo(ctx_);
}

utils::Expected<std::string, utils::Error> RequestDispatcher::HandleConfigHelp(const Command& cmd) {
  // Reference: ../mygram-db/src/server/handlers/admin_handler.cpp:HandleConfigHelp
  // Reusability: 100%
  return AdminHandler::HandleConfigHelp(cmd.path);
}

utils::Expected<std::string, utils::Error> RequestDispatcher::HandleConfigShow(const Command& cmd) {
  // Reference: ../mygram-db/src/server/handlers/admin_handler.cpp:HandleConfigShow
  // Reusability: 100%

  // Build ServerContext from HandlerContext
  // Reference: ../mygram-db/src/server/handlers/admin_handler.cpp for atomic load pattern
  ServerContext server_ctx;
  server_ctx.config = ctx_.config;

  // Server statistics (atomic loads are thread-safe)
  server_ctx.uptime_seconds = ctx_.stats.GetUptimeSeconds();
  server_ctx.connections_total = ctx_.stats.total_connections.load();
  server_ctx.connections_current = ctx_.stats.active_connections.load();
  server_ctx.queries_total = ctx_.stats.total_commands.load();
  server_ctx.queries_per_second = ctx_.stats.GetQueriesPerSecond();

  // Vector store statistics
  if (ctx_.vector_store != nullptr) {
    server_ctx.vectors_total = ctx_.vector_store->GetVectorCount();
    server_ctx.vector_dimension = static_cast<uint32_t>(ctx_.vector_store->GetDimension());
  }

  // Event store statistics
  if (ctx_.event_store != nullptr) {
    server_ctx.contexts_total = ctx_.event_store->GetContextCount();
    server_ctx.events_total = ctx_.event_store->GetTotalEventCount();
  }

  // Cache statistics
  server_ctx.cache_enabled = (ctx_.cache != nullptr);
  if (ctx_.cache != nullptr) {
    auto cache_stats = ctx_.cache->GetStatistics();
    server_ctx.cache_hits = cache_stats.cache_hits;
    server_ctx.cache_misses = cache_stats.cache_misses;
  }

  return AdminHandler::HandleConfigShow(server_ctx, cmd.path);
}

utils::Expected<std::string, utils::Error> RequestDispatcher::HandleConfigVerify(const Command& cmd) {
  // Reference: ../mygram-db/src/server/handlers/admin_handler.cpp:HandleConfigVerify
  // Reusability: 100%
  return AdminHandler::HandleConfigVerify(cmd.path);
}

utils::Expected<std::string, utils::Error> RequestDispatcher::HandleDumpSave(const Command& cmd) {
  // Reference: ../mygram-db/src/server/handlers/dump_handler.cpp::HandleDumpSave
  // Reusability: 90% (removed MySQL SYNC check)

  // Determine filepath
  std::string filepath;
  if (!cmd.path.empty()) {
    filepath = cmd.path;
    if (filepath[0] != '/') {
      filepath = ctx_.dump_dir + "/" + filepath;
    }
    // Canonicalize path and validate it's within dump_dir
    try {
      std::filesystem::path canonical = std::filesystem::weakly_canonical(filepath);
      std::filesystem::path dump_canonical = std::filesystem::weakly_canonical(ctx_.dump_dir);
      auto rel = canonical.lexically_relative(dump_canonical);
      if (rel.empty() || rel.string().substr(0, 2) == "..") {
        return utils::MakeUnexpected(
            utils::MakeError(utils::ErrorCode::kInvalidArgument, "Invalid filepath: path traversal detected"));
      }
    } catch (const std::exception& e) {
      return utils::MakeUnexpected(
          utils::MakeError(utils::ErrorCode::kInvalidArgument, std::string("Invalid filepath: ") + e.what()));
    }
  } else {
    // Generate default filename with timestamp
    auto now = std::time(nullptr);
    std::tm tm_buf{};
    localtime_r(&now, &tm_buf);  // Thread-safe version of localtime
    std::array<char, kFilepathBufferSize> buf{};
    std::strftime(buf.data(), buf.size(), "snapshot_%Y%m%d_%H%M%S.dmp", &tm_buf);
    filepath = ctx_.dump_dir + "/" + std::string(buf.data());
  }

  utils::LogStorageInfo("dump_save", "Attempting to save snapshot to: " + filepath);

  // Check if config is available
  if (ctx_.config == nullptr) {
    std::string error_msg = "Cannot save snapshot: server configuration is not available";
    utils::LogStorageError("dump_save", filepath, error_msg);
    return utils::MakeUnexpected(utils::MakeError(utils::ErrorCode::kInternalError, error_msg));
  }

  // Check required stores
  if (ctx_.event_store == nullptr || ctx_.co_index == nullptr || ctx_.vector_store == nullptr) {
    std::string error_msg = "Cannot save snapshot: required stores not initialized";
    utils::LogStorageError("dump_save", filepath, error_msg);
    return utils::MakeUnexpected(utils::MakeError(utils::ErrorCode::kInternalError, error_msg));
  }

  // Set read-only mode (RAII guard ensures it's cleared even on exceptions)
  FlagGuard read_only_guard(ctx_.read_only);

  // Call snapshot_v1 API
  auto result = storage::snapshot_v1::WriteSnapshotV1(filepath, *ctx_.config, *ctx_.event_store, *ctx_.co_index,
                                                       *ctx_.vector_store);

  if (result) {
    utils::LogStorageInfo("dump_save", "Successfully saved snapshot to: " + filepath);
    return FormatOK("DUMP_SAVED " + filepath);
  }

  std::string error_msg = "Failed to save snapshot to " + filepath + ": " + result.error().message();
  utils::LogStorageError("dump_save", filepath, result.error().message());
  return utils::MakeUnexpected(
      utils::MakeError(utils::ErrorCode::kSnapshotSaveFailed, error_msg));
}

utils::Expected<std::string, utils::Error> RequestDispatcher::HandleDumpLoad(const Command& cmd) {
  // Reference: ../mygram-db/src/server/handlers/dump_handler.cpp::HandleDumpLoad
  // Reusability: 90% (removed MySQL SYNC check)

  std::string filepath;
  if (!cmd.path.empty()) {
    filepath = cmd.path;
    if (filepath[0] != '/') {
      filepath = ctx_.dump_dir + "/" + filepath;
    }
    // Canonicalize path and validate it's within dump_dir
    try {
      std::filesystem::path canonical = std::filesystem::weakly_canonical(filepath);
      std::filesystem::path dump_canonical = std::filesystem::weakly_canonical(ctx_.dump_dir);
      auto rel = canonical.lexically_relative(dump_canonical);
      if (rel.empty() || rel.string().substr(0, 2) == "..") {
        return utils::MakeUnexpected(
            utils::MakeError(utils::ErrorCode::kInvalidArgument, "Invalid filepath: path traversal detected"));
      }
    } catch (const std::exception& e) {
      return utils::MakeUnexpected(
          utils::MakeError(utils::ErrorCode::kInvalidArgument, std::string("Invalid filepath: ") + e.what()));
    }
  } else {
    return utils::MakeUnexpected(
        utils::MakeError(utils::ErrorCode::kInvalidArgument, "DUMP LOAD requires a filepath"));
  }

  utils::LogStorageInfo("dump_load", "Attempting to load snapshot from: " + filepath);

  // Check required stores
  if (ctx_.event_store == nullptr || ctx_.co_index == nullptr || ctx_.vector_store == nullptr) {
    std::string error_msg = "Cannot load snapshot: required stores not initialized";
    utils::LogStorageError("dump_load", filepath, error_msg);
    return utils::MakeUnexpected(utils::MakeError(utils::ErrorCode::kInternalError, error_msg));
  }

  // Set loading mode (RAII guard ensures it's cleared even on exceptions)
  FlagGuard loading_guard(ctx_.loading);

  // Variables to receive loaded data
  config::Config loaded_config;
  storage::snapshot_format::IntegrityError integrity_error;

  // Call snapshot_v1 API
  auto result = storage::snapshot_v1::ReadSnapshotV1(filepath, loaded_config, *ctx_.event_store, *ctx_.co_index,
                                                      *ctx_.vector_store, nullptr, nullptr, &integrity_error);

  if (result) {
    utils::LogStorageInfo("dump_load", "Successfully loaded snapshot from: " + filepath);
    return FormatOK("DUMP_LOADED " + filepath);
  }

  std::string error_msg = "Failed to load snapshot from " + filepath + ": " + result.error().message();
  if (!integrity_error.message.empty()) {
    error_msg += " (" + integrity_error.message + ")";
  }
  utils::LogStorageError("dump_load", filepath, error_msg);
  return utils::MakeUnexpected(
      utils::MakeError(utils::ErrorCode::kSnapshotLoadFailed, error_msg));
}

utils::Expected<std::string, utils::Error> RequestDispatcher::HandleDumpVerify(const Command& cmd) {
  // Reference: ../mygram-db/src/server/handlers/dump_handler.cpp::HandleDumpVerify
  // Reusability: 95%

  std::string filepath;
  if (!cmd.path.empty()) {
    filepath = cmd.path;
    if (filepath[0] != '/') {
      filepath = ctx_.dump_dir + "/" + filepath;
    }
    // Canonicalize path and validate it's within dump_dir
    try {
      std::filesystem::path canonical = std::filesystem::weakly_canonical(filepath);
      std::filesystem::path dump_canonical = std::filesystem::weakly_canonical(ctx_.dump_dir);
      auto rel = canonical.lexically_relative(dump_canonical);
      if (rel.empty() || rel.string().substr(0, 2) == "..") {
        return utils::MakeUnexpected(
            utils::MakeError(utils::ErrorCode::kInvalidArgument, "Invalid filepath: path traversal detected"));
      }
    } catch (const std::exception& e) {
      return utils::MakeUnexpected(
          utils::MakeError(utils::ErrorCode::kInvalidArgument, std::string("Invalid filepath: ") + e.what()));
    }
  } else {
    return utils::MakeUnexpected(
        utils::MakeError(utils::ErrorCode::kInvalidArgument, "DUMP VERIFY requires a filepath"));
  }

  utils::LogStorageInfo("dump_verify", "Verifying snapshot: " + filepath);

  storage::snapshot_format::IntegrityError integrity_error;
  auto result = storage::snapshot_v1::VerifySnapshotIntegrity(filepath, integrity_error);

  if (result) {
    utils::LogStorageInfo("dump_verify", "Snapshot verification succeeded: " + filepath);
    return "OK DUMP_VERIFIED " + filepath + "\r\n";
  }

  std::string error_msg = "Snapshot verification failed for " + filepath + ": " + result.error().message();
  if (!integrity_error.message.empty()) {
    error_msg += " (" + integrity_error.message + ")";
  }
  utils::LogStorageError("dump_verify", filepath, error_msg);
  return utils::MakeUnexpected(
      utils::MakeError(utils::ErrorCode::kSnapshotVerifyFailed, error_msg));
}

utils::Expected<std::string, utils::Error> RequestDispatcher::HandleDumpInfo(const Command& cmd) {
  // Reference: ../mygram-db/src/server/handlers/dump_handler.cpp::HandleDumpInfo
  // Reusability: 90% (removed GTID, changed table_count to store_count)

  std::string filepath;
  if (!cmd.path.empty()) {
    filepath = cmd.path;
    if (filepath[0] != '/') {
      filepath = ctx_.dump_dir + "/" + filepath;
    }
    // Canonicalize path and validate it's within dump_dir
    try {
      std::filesystem::path canonical = std::filesystem::weakly_canonical(filepath);
      std::filesystem::path dump_canonical = std::filesystem::weakly_canonical(ctx_.dump_dir);
      auto rel = canonical.lexically_relative(dump_canonical);
      if (rel.empty() || rel.string().substr(0, 2) == "..") {
        return utils::MakeUnexpected(
            utils::MakeError(utils::ErrorCode::kInvalidArgument, "Invalid filepath: path traversal detected"));
      }
    } catch (const std::exception& e) {
      return utils::MakeUnexpected(
          utils::MakeError(utils::ErrorCode::kInvalidArgument, std::string("Invalid filepath: ") + e.what()));
    }
  } else {
    return utils::MakeUnexpected(
        utils::MakeError(utils::ErrorCode::kInvalidArgument, "DUMP INFO requires a filepath"));
  }

  utils::LogStorageInfo("dump_info", "Reading snapshot info: " + filepath);

  storage::snapshot_v1::SnapshotInfo info;
  auto info_result = storage::snapshot_v1::GetSnapshotInfo(filepath, info);

  if (!info_result) {
    return utils::MakeUnexpected(utils::MakeError(utils::ErrorCode::kSnapshotInfoFailed,
                                                   "Failed to read snapshot info from " + filepath + ": " +
                                                       info_result.error().message()));
  }

  std::ostringstream result;
  result << "OK DUMP_INFO " << filepath << "\r\n";
  result << "version: " << info.version << "\r\n";
  result << "stores: " << info.store_count << "\r\n";
  result << "flags: " << info.flags << "\r\n";
  result << "file_size: " << info.file_size << "\r\n";
  result << "timestamp: " << info.timestamp << "\r\n";
  result << "has_statistics: " << (info.has_statistics ? "true" : "false") << "\r\n";
  result << "END\r\n";

  return result.str();
}

utils::Expected<std::string, utils::Error> RequestDispatcher::HandleDebugOn(ConnectionContext& conn_ctx) {
  return handlers::HandleDebugOn(conn_ctx);
}

utils::Expected<std::string, utils::Error> RequestDispatcher::HandleDebugOff(ConnectionContext& conn_ctx) {
  return handlers::HandleDebugOff(conn_ctx);
}

utils::Expected<std::string, utils::Error> RequestDispatcher::HandleCacheStats(const Command& /* cmd */) {
  if (ctx_.cache == nullptr) {
    return utils::MakeUnexpected(utils::MakeError(utils::ErrorCode::kCacheDisabled, "Cache is disabled"));
  }

  auto stats = ctx_.cache->GetStatistics();

  // Format stats (Redis-style)
  std::ostringstream oss;
  oss << "OK CACHE_STATS\r\n";
  oss << "total_queries: " << stats.total_queries << "\r\n";
  oss << "cache_hits: " << stats.cache_hits << "\r\n";
  oss << "cache_misses: " << stats.cache_misses << "\r\n";
  oss << "cache_misses_invalidated: " << stats.cache_misses_invalidated << "\r\n";
  oss << "cache_misses_not_found: " << stats.cache_misses_not_found << "\r\n";
  oss << "hit_rate: " << std::fixed << std::setprecision(4) << stats.HitRate() << "\r\n";
  oss << "current_entries: " << stats.current_entries << "\r\n";
  oss << "current_memory_bytes: " << stats.current_memory_bytes << "\r\n";
  oss << "current_memory_mb: " << std::fixed << std::setprecision(2)
      << (stats.current_memory_bytes / (1024.0 * 1024.0)) << "\r\n";
  oss << "evictions: " << stats.evictions << "\r\n";
  oss << "avg_hit_latency_ms: " << std::fixed << std::setprecision(3) << stats.AverageCacheHitLatency() << "\r\n";
  oss << "avg_miss_latency_ms: " << std::fixed << std::setprecision(3) << stats.AverageCacheMissLatency() << "\r\n";
  oss << "time_saved_ms: " << std::fixed << std::setprecision(2) << stats.TotalTimeSaved() << "\r\n";

  return oss.str();
}

utils::Expected<std::string, utils::Error> RequestDispatcher::HandleCacheClear(const Command& /* cmd */) {
  if (ctx_.cache == nullptr) {
    return utils::MakeUnexpected(utils::MakeError(utils::ErrorCode::kCacheDisabled, "Cache is disabled"));
  }

  ctx_.cache->Clear();
  return FormatOK("CACHE CLEARED");
}

utils::Expected<std::string, utils::Error> RequestDispatcher::HandleCacheEnable(const Command& /* cmd */) {
  if (ctx_.cache == nullptr) {
    return utils::MakeUnexpected(
        utils::MakeError(utils::ErrorCode::kCacheDisabled, "Cache was not initialized at startup"));
  }

  // Cache is always enabled if it was initialized
  // This is a no-op, but kept for API compatibility
  return FormatOK("CACHE ENABLED");
}

utils::Expected<std::string, utils::Error> RequestDispatcher::HandleCacheDisable(const Command& /* cmd */) {
  if (ctx_.cache == nullptr) {
    return utils::MakeUnexpected(utils::MakeError(utils::ErrorCode::kCacheDisabled, "Cache is already disabled"));
  }

  // For now, we don't support runtime disable (cache is always active if initialized)
  // To disable, restart server with cache.enabled=false in config
  return utils::MakeUnexpected(utils::MakeError(
      utils::ErrorCode::kNotImplemented,
      "Runtime cache disable not supported. Set cache.enabled=false in config and restart."));
}

//
// Format helpers
//

std::string RequestDispatcher::FormatOK(const std::string& msg) const {
  if (msg.empty()) {
    return "OK\r\n";
  }
  return "OK " + msg + "\r\n";
}

std::string RequestDispatcher::FormatError(const std::string& msg) const { return "ERROR " + msg + "\r\n"; }

std::string RequestDispatcher::FormatSimResults(const std::vector<std::pair<std::string, float>>& results,
                                                 int count) const {
  std::ostringstream oss;
  oss << "OK RESULTS " << count;
  for (const auto& [id, score] : results) {
    oss << " " << id << " " << score;
  }
  return oss.str();
}

}  // namespace nvecd::server
