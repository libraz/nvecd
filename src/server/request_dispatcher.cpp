/**
 * @file request_dispatcher.cpp
 * @brief Implementation of RequestDispatcher
 *
 * Reference: ../mygram-db/src/server/request_dispatcher.cpp
 * Reusability: 75% (similar dispatch pattern, different handlers)
 */

#include <sstream>
#include <array>
#include <ctime>
#include <filesystem>

// Include concrete types before request_dispatcher.h to resolve forward declarations
#include "events/co_occurrence_index.h"
#include "events/event_store.h"
#include "similarity/similarity_engine.h"
#include "vectors/vector_store.h"

#include "server/request_dispatcher.h"
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
      result = HandleInfo(*cmd);
      break;

    case CommandType::kConfigHelp:
      result = HandleConfigHelp(*cmd);
      break;

    case CommandType::kConfigShow:
      result = HandleConfigShow(*cmd);
      break;

    case CommandType::kConfigVerify:
      result = HandleConfigVerify(*cmd);
      break;

    case CommandType::kDumpSave:
      result = HandleDumpSave(*cmd);
      break;

    case CommandType::kDumpLoad:
      result = HandleDumpLoad(*cmd);
      break;

    case CommandType::kDumpVerify:
      result = HandleDumpVerify(*cmd);
      break;

    case CommandType::kDumpInfo:
      result = HandleDumpInfo(*cmd);
      break;

    case CommandType::kDebugOn:
      result = HandleDebugOn(conn_ctx);
      break;

    case CommandType::kDebugOff:
      result = HandleDebugOff(conn_ctx);
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

  return FormatOK("VECSET");
}

utils::Expected<std::string, utils::Error> RequestDispatcher::HandleSim(const Command& cmd,
                                                                         ConnectionContext& conn_ctx) {
  (void)conn_ctx;  // TODO: Use for debug mode
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

  auto result = ctx_.similarity_engine->SearchByVector(cmd.vector, cmd.top_k);
  if (!result) {
    return utils::MakeUnexpected(result.error());
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
  (void)cmd;  // Unused parameter
  // TODO: Implement CONFIG HELP
  return FormatOK("CONFIG HELP not yet implemented");
}

utils::Expected<std::string, utils::Error> RequestDispatcher::HandleConfigShow(const Command& cmd) {
  (void)cmd;  // Unused parameter
  // TODO: Implement CONFIG SHOW
  return FormatOK("CONFIG SHOW not yet implemented");
}

utils::Expected<std::string, utils::Error> RequestDispatcher::HandleConfigVerify(const Command& cmd) {
  (void)cmd;  // Unused parameter
  // TODO: Implement CONFIG VERIFY
  return FormatOK("Configuration is valid (not yet fully implemented)");
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

  // Check if full_config is available
  if (ctx_.full_config == nullptr) {
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
  auto result = storage::snapshot_v1::WriteSnapshotV1(filepath, *ctx_.full_config, *ctx_.event_store, *ctx_.co_index,
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
  oss << "OK RESULTS " << count << "\r\n";
  for (const auto& [id, score] : results) {
    oss << id << " " << score << "\r\n";
  }
  return oss.str();
}

}  // namespace nvecd::server
