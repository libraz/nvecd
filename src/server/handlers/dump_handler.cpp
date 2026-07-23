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
#include <filesystem>
#include <memory>
#include <shared_mutex>
#include <sstream>

#include "cache/similarity_cache.h"
#include "config/config.h"
#include "events/co_occurrence_index.h"
#include "events/event_store.h"
#include "similarity/similarity_engine.h"
#include "storage/snapshot_fork.h"
#include "storage/snapshot_format_v1.h"
#include "storage/snapshot_lock.h"
#include "storage/wal.h"
#include "storage/wal_checkpoint.h"
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

  if (ctx.loading.load(std::memory_order_acquire)) {
    return utils::MakeUnexpected(utils::MakeError(utils::ErrorCode::kSnapshotAlreadyInProgress,
                                                  "Cannot save snapshot while a snapshot load is in progress"));
  }

  // Branch on snapshot mode
  if (ctx.config->snapshot.mode == "fork") {
    // Fork mode: non-blocking background save
    if (ctx.fork_snapshot_writer == nullptr) {
      return utils::MakeUnexpected(
          utils::MakeError(utils::ErrorCode::kInternalError, "Fork snapshot writer not initialized"));
    }

    // Serialize the pre-fork store barrier and WAL sequence capture with both
    // regular mutations and DUMP LOAD. The guard only covers fork(), not the
    // child write, so serving resumes immediately after the COW image exists.
    std::shared_lock<std::shared_mutex> snapshot_write_guard;
    if (ctx.snapshot_write_gate != nullptr) {
      snapshot_write_guard = std::shared_lock(*ctx.snapshot_write_gate);
    }
    if (ctx.loading.load(std::memory_order_acquire)) {
      return utils::MakeUnexpected(utils::MakeError(utils::ErrorCode::kSnapshotAlreadyInProgress,
                                                    "Cannot save snapshot while a snapshot load is in progress"));
    }
    std::unique_lock<std::mutex> write_serialization_guard;
    if (ctx.write_serialization_gate != nullptr) {
      write_serialization_guard = std::unique_lock(*ctx.write_serialization_gate);
    }

    // Reap any finished child first
    ctx.fork_snapshot_writer->CheckChild();

    auto result = ctx.fork_snapshot_writer->StartBackgroundSave(resolved_path, *ctx.config, *ctx.event_store,
                                                                *ctx.co_index, *ctx.vector_store, ctx.metadata_store);

    if (!result) {
      utils::LogStorageError("dump_save", resolved_path, result.error().message());
      return utils::MakeUnexpected(result.error());
    }

    utils::LogStorageInfo("dump_save", "Background snapshot started: " + resolved_path);
    return std::string("OK DUMP_SAVE_STARTED " + resolved_path + "\r\n");

  } else {
    // Lock mode: synchronous blocking save
    bool expected = false;
    if (!ctx.read_only.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
      return utils::MakeUnexpected(utils::MakeError(utils::ErrorCode::kSnapshotAlreadyInProgress,
                                                    "Another snapshot save is already in progress"));
    }
    utils::FlagResetGuard read_only_guard(ctx.read_only);
    // Publish read_only before taking the exclusive gate. Existing writers
    // drain under their shared guards; writers arriving afterwards block, then
    // observe read_only and fail without crossing the snapshot boundary.
    std::unique_lock<std::shared_mutex> snapshot_write_guard;
    if (ctx.snapshot_write_gate != nullptr) {
      snapshot_write_guard = std::unique_lock(*ctx.snapshot_write_gate);
    }

    uint64_t captured_wal_sequence = 0;
    auto result =
        storage::WriteSnapshotWithLock(resolved_path, *ctx.config, *ctx.event_store, *ctx.co_index, *ctx.vector_store,
                                       nullptr, nullptr, ctx.metadata_store, ctx.wal, &captured_wal_sequence);

    if (result) {
      utils::LogStorageInfo("dump_save", "Successfully saved snapshot to: " + resolved_path);
      // Record the checkpoint sidecar then truncate the WAL up to the sequence
      // captured under the snapshot's write-lock barrier. Skipped entirely when
      // the WAL is disabled (ctx.wal == nullptr).
      if (ctx.wal != nullptr) {
        auto checkpoint = storage::WriteWalCheckpoint(resolved_path, captured_wal_sequence);
        if (!checkpoint) {
          utils::LogStorageError("dump_save", resolved_path,
                                 "Failed to write WAL checkpoint: " + checkpoint.error().message());
        } else {
          auto truncated = ctx.wal->Truncate(captured_wal_sequence);
          if (!truncated) {
            utils::LogStorageError("dump_save", resolved_path,
                                   "Failed to truncate WAL: " + truncated.error().message());
          }
        }
      }
      return std::string("OK DUMP_SAVED " + resolved_path + "\r\n");
    }

    std::string error_msg = "Failed to save snapshot to " + resolved_path + ": " + result.error().message();
    utils::LogStorageError("dump_save", resolved_path, result.error().message());
    return utils::MakeUnexpected(utils::MakeError(utils::ErrorCode::kSnapshotSaveFailed, error_msg));
  }
}

utils::Expected<std::string, utils::Error> HandleDumpLoad(HandlerContext& ctx, const std::string& filepath) {
  if (filepath.empty()) {
    return utils::MakeUnexpected(utils::MakeError(utils::ErrorCode::kInvalidArgument, "DUMP LOAD requires a filepath"));
  }

  auto validated = utils::ValidateDumpPath(filepath, ctx.dump_dir);
  if (!validated) {
    return utils::MakeUnexpected(validated.error());
  }
  std::string resolved_path = *validated;

  utils::LogStorageInfo("dump_load", "Attempting to load snapshot from: " + resolved_path);

  // Check required stores
  if (ctx.config == nullptr || ctx.event_store == nullptr || ctx.co_index == nullptr || ctx.vector_store == nullptr) {
    std::string error_msg = "Cannot load snapshot: required stores not initialized";
    utils::LogStorageError("dump_load", resolved_path, error_msg);
    return utils::MakeUnexpected(utils::MakeError(utils::ErrorCode::kInternalError, error_msg));
  }

  // Reserve the load operation before waiting for the lifecycle gates. This
  // makes concurrent LOAD calls fail deterministically instead of allowing two
  // deserializers to clear and repopulate the same live stores.
  bool expected = false;
  if (!ctx.loading.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
    return utils::MakeUnexpected(
        utils::MakeError(utils::ErrorCode::kSnapshotAlreadyInProgress, "Another snapshot load is already in progress"));
  }
  utils::FlagResetGuard loading_guard(ctx.loading);

  if (ctx.read_only.load(std::memory_order_acquire)) {
    return utils::MakeUnexpected(
        utils::MakeError(utils::ErrorCode::kSnapshotAlreadyInProgress, "A snapshot save is already in progress"));
  }

  // Deserialize into isolated stores first. A corrupt or semantically invalid
  // snapshot can mutate only these staged instances, never the live state.
  events::CoOccurrenceIndex::Config co_index_config;
  co_index_config.max_neighbors_per_item = ctx.config->events.max_neighbors_per_item;
  co_index_config.min_support = static_cast<float>(ctx.config->events.min_support);
  events::EventStore staged_event_store(ctx.config->events);
  events::CoOccurrenceIndex staged_co_index(co_index_config);
  vectors::VectorStore staged_vector_store(ctx.config->vectors);
  auto staged_metadata_store = ctx.metadata_store != nullptr ? std::make_unique<vectors::MetadataStore>() : nullptr;
  config::Config loaded_config;
  storage::snapshot_format::IntegrityError integrity_error;
  auto result = storage::snapshot_v1::ReadSnapshotV1(resolved_path, loaded_config, staged_event_store, staged_co_index,
                                                     staged_vector_store, nullptr, nullptr, &integrity_error,
                                                     staged_metadata_store.get());

  if (!result) {
    std::string error_msg = "Failed to load snapshot from " + resolved_path + ": " + result.error().message();
    if (!integrity_error.message.empty()) {
      error_msg += " (" + integrity_error.message + ")";
    }
    utils::LogStorageError("dump_load", resolved_path, error_msg);
    return utils::MakeUnexpected(utils::MakeError(utils::ErrorCode::kSnapshotLoadFailed, error_msg));
  }

  // Drain all mutations and similarity queries only for the short publication
  // phase. The lock order mirrors normal requests: snapshot gate, then WAL
  // serialization gate. New requests observe loading=true and fail promptly.
  std::unique_lock<std::shared_mutex> snapshot_write_guard;
  if (ctx.snapshot_write_gate != nullptr) {
    snapshot_write_guard = std::unique_lock(*ctx.snapshot_write_gate);
  }
  std::unique_lock<std::mutex> write_serialization_guard;
  if (ctx.write_serialization_gate != nullptr) {
    write_serialization_guard = std::unique_lock(*ctx.write_serialization_gate);
  }

  ctx.event_store->SwapState(staged_event_store);
  ctx.co_index->SwapState(staged_co_index);
  ctx.vector_store->SwapState(staged_vector_store);
  if (ctx.metadata_store != nullptr) {
    ctx.metadata_store->SwapState(*staged_metadata_store);
  }

  if (ctx.similarity_engine != nullptr) {
    ctx.similarity_engine->RebuildAnnFromStore();
  }
  ctx.vector_generation.fetch_add(1, std::memory_order_acq_rel);
  ctx.metadata_generation.fetch_add(1, std::memory_order_acq_rel);
  ctx.dataset_generation.fetch_add(1, std::memory_order_acq_rel);
  auto* cache_ptr = ctx.cache.load(std::memory_order_acquire);
  if (cache_ptr != nullptr) {
    cache_ptr->Clear();
  }

  auto fail_stop = [&](const utils::Error& error) -> utils::Expected<std::string, utils::Error> {
    // Publication has already occurred. If any durability step fails, do not
    // reopen the server on a state whose recovery base is uncertain. Keep both
    // lifecycle flags set until an operator restarts after fixing storage.
    ctx.read_only.store(true, std::memory_order_release);
    ctx.loading.store(true, std::memory_order_release);
    loading_guard.KeepSet();
    utils::LogStorageError("dump_load_fail_stop", resolved_path, error.message());
    return utils::MakeUnexpected(error);
  };

  {
    if (ctx.wal != nullptr) {
      // DUMP LOAD is an explicit rollback to this snapshot. Make that snapshot
      // the durable recovery base and discard the pre-load WAL tail; otherwise
      // a later restart would replay mutations that the operator deliberately
      // rolled back.
      const uint64_t checkpoint_sequence = ctx.wal->CurrentSequence();
      std::error_code ec;
      std::filesystem::last_write_time(resolved_path, std::filesystem::file_time_type::clock::now(), ec);
      if (ec) {
        return fail_stop(utils::MakeError(utils::ErrorCode::kStorageWriteError,
                                          "Failed to mark loaded snapshot as recovery base: " + ec.message()));
      }
      auto checkpoint = storage::WriteWalCheckpoint(resolved_path, checkpoint_sequence);
      if (!checkpoint) {
        return fail_stop(checkpoint.error());
      }
      auto truncated = ctx.wal->Truncate(checkpoint_sequence);
      if (!truncated) {
        return fail_stop(truncated.error());
      }
    }
    utils::LogStorageInfo("dump_load", "Successfully loaded snapshot from: " + resolved_path);
    return std::string("OK DUMP_LOADED " + resolved_path + "\r\n");
  }
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
    return utils::MakeUnexpected(utils::MakeError(utils::ErrorCode::kInvalidArgument, "DUMP INFO requires a filepath"));
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
    return utils::MakeUnexpected(
        utils::MakeError(utils::ErrorCode::kSnapshotInfoFailed,
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

utils::Expected<std::string, utils::Error> HandleDumpStatus(HandlerContext& ctx) {
  if (ctx.fork_snapshot_writer == nullptr) {
    // No fork writer — return idle status
    return std::string("OK DUMP_STATUS\r\nstatus: idle\r\nEND\r\n");
  }

  // Reap any finished child
  ctx.fork_snapshot_writer->CheckChild();

  auto status = ctx.fork_snapshot_writer->GetStatus();

  std::ostringstream result;
  result << "OK DUMP_STATUS\r\n";

  switch (status.status) {
    case storage::SnapshotStatus::kIdle:
      result << "status: idle\r\n";
      break;
    case storage::SnapshotStatus::kInProgress:
      result << "status: in_progress\r\n";
      result << "filepath: " << status.filepath << "\r\n";
      result << "pid: " << status.child_pid << "\r\n";
      result << "start_time: " << status.start_time << "\r\n";
      break;
    case storage::SnapshotStatus::kCompleted:
      result << "status: completed\r\n";
      result << "filepath: " << status.filepath << "\r\n";
      result << "start_time: " << status.start_time << "\r\n";
      result << "end_time: " << status.end_time << "\r\n";
      break;
    case storage::SnapshotStatus::kFailed:
      result << "status: failed\r\n";
      result << "filepath: " << status.filepath << "\r\n";
      result << "start_time: " << status.start_time << "\r\n";
      result << "end_time: " << status.end_time << "\r\n";
      result << "error: " << status.error_message << "\r\n";
      break;
  }

  result << "END\r\n";
  return result.str();
}

}  // namespace nvecd::server::handlers
