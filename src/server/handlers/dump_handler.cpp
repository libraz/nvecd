/**
 * @file dump_handler.cpp
 * @brief DUMP command handler implementations
 *
 * Reference: ../mygram-db/src/server/handlers/dump_handler.cpp
 * Reusability: 90% (removed MySQL SYNC check)
 */

#include "server/handlers/dump_handler.h"

#include <array>
#include <ctime>
#include <sstream>

#include "config/config.h"
#include "events/co_occurrence_index.h"
#include "events/event_store.h"
#include "storage/snapshot_format_v1.h"
#include "utils/flag_guard.h"
#include "utils/path_utils.h"
#include "utils/structured_log.h"
#include "vectors/vector_store.h"

namespace nvecd::server::handlers {

namespace {
constexpr int kFilepathBufferSize = 256;
}  // namespace

utils::Expected<std::string, utils::Error> HandleDumpSave(HandlerContext& ctx, const std::string& filepath) {
  std::string resolved_path;

  if (!filepath.empty()) {
    auto validated = utils::ValidateDumpPath(filepath, ctx.dump_dir);
    if (!validated) {
      return utils::MakeUnexpected(validated.error());
    }
    resolved_path = *validated;
  } else {
    // Generate default filename with timestamp
    auto now = std::time(nullptr);
    std::tm tm_buf{};
    localtime_r(&now, &tm_buf);  // Thread-safe version of localtime
    std::array<char, kFilepathBufferSize> buf{};
    std::strftime(buf.data(), buf.size(), "snapshot_%Y%m%d_%H%M%S.dmp", &tm_buf);
    resolved_path = ctx.dump_dir + "/" + std::string(buf.data());
  }

  utils::LogStorageInfo("dump_save", "Attempting to save snapshot to: " + resolved_path);

  // Check if config is available
  if (ctx.config == nullptr) {
    std::string error_msg = "Cannot save snapshot: server configuration is not available";
    utils::LogStorageError("dump_save", resolved_path, error_msg);
    return utils::MakeUnexpected(utils::MakeError(utils::ErrorCode::kInternalError, error_msg));
  }

  // Check required stores
  if (ctx.event_store == nullptr || ctx.co_index == nullptr || ctx.vector_store == nullptr) {
    std::string error_msg = "Cannot save snapshot: required stores not initialized";
    utils::LogStorageError("dump_save", resolved_path, error_msg);
    return utils::MakeUnexpected(utils::MakeError(utils::ErrorCode::kInternalError, error_msg));
  }

  // Set read-only mode (RAII guard ensures it's cleared)
  utils::FlagGuard read_only_guard(ctx.read_only);

  // Call snapshot_v1 API
  auto result = storage::snapshot_v1::WriteSnapshotV1(resolved_path, *ctx.config, *ctx.event_store, *ctx.co_index,
                                                      *ctx.vector_store);

  if (result) {
    utils::LogStorageInfo("dump_save", "Successfully saved snapshot to: " + resolved_path);
    return std::string("OK DUMP_SAVED " + resolved_path + "\r\n");
  }

  std::string error_msg = "Failed to save snapshot to " + resolved_path + ": " + result.error().message();
  utils::LogStorageError("dump_save", resolved_path, result.error().message());
  return utils::MakeUnexpected(utils::MakeError(utils::ErrorCode::kSnapshotSaveFailed, error_msg));
}

utils::Expected<std::string, utils::Error> HandleDumpLoad(HandlerContext& ctx, const std::string& filepath) {
  if (filepath.empty()) {
    return utils::MakeUnexpected(
        utils::MakeError(utils::ErrorCode::kInvalidArgument, "DUMP LOAD requires a filepath"));
  }

  auto validated = utils::ValidateDumpPath(filepath, ctx.dump_dir);
  if (!validated) {
    return utils::MakeUnexpected(validated.error());
  }
  std::string resolved_path = *validated;

  utils::LogStorageInfo("dump_load", "Attempting to load snapshot from: " + resolved_path);

  // Check required stores
  if (ctx.event_store == nullptr || ctx.co_index == nullptr || ctx.vector_store == nullptr) {
    std::string error_msg = "Cannot load snapshot: required stores not initialized";
    utils::LogStorageError("dump_load", resolved_path, error_msg);
    return utils::MakeUnexpected(utils::MakeError(utils::ErrorCode::kInternalError, error_msg));
  }

  // Set loading mode (RAII guard ensures it's cleared)
  utils::FlagGuard loading_guard(ctx.loading);

  // Variables to receive loaded data
  config::Config loaded_config;
  storage::snapshot_format::IntegrityError integrity_error;

  // Call snapshot_v1 API
  auto result = storage::snapshot_v1::ReadSnapshotV1(resolved_path, loaded_config, *ctx.event_store, *ctx.co_index,
                                                     *ctx.vector_store, nullptr, nullptr, &integrity_error);

  if (result) {
    utils::LogStorageInfo("dump_load", "Successfully loaded snapshot from: " + resolved_path);
    return std::string("OK DUMP_LOADED " + resolved_path + "\r\n");
  }

  std::string error_msg = "Failed to load snapshot from " + resolved_path + ": " + result.error().message();
  if (!integrity_error.message.empty()) {
    error_msg += " (" + integrity_error.message + ")";
  }
  utils::LogStorageError("dump_load", resolved_path, error_msg);
  return utils::MakeUnexpected(utils::MakeError(utils::ErrorCode::kSnapshotLoadFailed, error_msg));
}

utils::Expected<std::string, utils::Error> HandleDumpVerify(const std::string& dump_dir, const std::string& filepath) {
  if (filepath.empty()) {
    return utils::MakeUnexpected(
        utils::MakeError(utils::ErrorCode::kInvalidArgument, "DUMP VERIFY requires a filepath"));
  }

  auto validated = utils::ValidateDumpPath(filepath, dump_dir);
  if (!validated) {
    return utils::MakeUnexpected(validated.error());
  }
  std::string resolved_path = *validated;

  utils::LogStorageInfo("dump_verify", "Verifying snapshot: " + resolved_path);

  storage::snapshot_format::IntegrityError integrity_error;
  auto result = storage::snapshot_v1::VerifySnapshotIntegrity(resolved_path, integrity_error);

  if (result) {
    utils::LogStorageInfo("dump_verify", "Snapshot verification succeeded: " + resolved_path);
    return std::string("OK DUMP_VERIFIED " + resolved_path + "\r\n");
  }

  std::string error_msg = "Snapshot verification failed for " + resolved_path + ": " + result.error().message();
  if (!integrity_error.message.empty()) {
    error_msg += " (" + integrity_error.message + ")";
  }
  utils::LogStorageError("dump_verify", resolved_path, error_msg);
  return utils::MakeUnexpected(utils::MakeError(utils::ErrorCode::kSnapshotVerifyFailed, error_msg));
}

utils::Expected<std::string, utils::Error> HandleDumpInfo(const std::string& dump_dir, const std::string& filepath) {
  if (filepath.empty()) {
    return utils::MakeUnexpected(
        utils::MakeError(utils::ErrorCode::kInvalidArgument, "DUMP INFO requires a filepath"));
  }

  auto validated = utils::ValidateDumpPath(filepath, dump_dir);
  if (!validated) {
    return utils::MakeUnexpected(validated.error());
  }
  std::string resolved_path = *validated;

  utils::LogStorageInfo("dump_info", "Reading snapshot info: " + resolved_path);

  storage::snapshot_v1::SnapshotInfo info;
  auto info_result = storage::snapshot_v1::GetSnapshotInfo(resolved_path, info);

  if (!info_result) {
    return utils::MakeUnexpected(utils::MakeError(
        utils::ErrorCode::kSnapshotInfoFailed,
        "Failed to read snapshot info from " + resolved_path + ": " + info_result.error().message()));
  }

  std::ostringstream result;
  result << "OK DUMP_INFO " << resolved_path << "\r\n";
  result << "version: " << info.version << "\r\n";
  result << "stores: " << info.store_count << "\r\n";
  result << "flags: " << info.flags << "\r\n";
  result << "file_size: " << info.file_size << "\r\n";
  result << "timestamp: " << info.timestamp << "\r\n";
  result << "has_statistics: " << (info.has_statistics ? "true" : "false") << "\r\n";
  result << "END\r\n";

  return result.str();
}

}  // namespace nvecd::server::handlers
