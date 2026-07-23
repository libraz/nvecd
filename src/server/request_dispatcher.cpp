/**
 * @file request_dispatcher.cpp
 * @brief Implementation of RequestDispatcher
 *
 * Reference: ../mygram-db/src/server/request_dispatcher.cpp
 * Reusability: 75% (similar dispatch pattern, different handlers)
 */

#include <spdlog/spdlog.h>

#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <sstream>

// Include concrete types before request_dispatcher.h to resolve forward declarations
#include "cache/cache_key.h"
#include "cache/cache_key_generator.h"
#include "cache/similarity_cache.h"
#include "events/co_occurrence_index.h"
#include "events/event_store.h"
#include "server/filter_parser.h"
#include "server/handlers/admin_handler.h"
#include "server/handlers/cache_handler.h"
#include "server/handlers/debug_handler.h"
#include "server/handlers/dump_handler.h"
#include "server/handlers/info_handler.h"
#include "server/handlers/variable_handler.h"
#include "server/request_dispatcher.h"
#include "server/score_format.h"
#include "server/similarity_result_utils.h"
#include "server/wal_codec.h"
#include "similarity/similarity_engine.h"
#include "storage/wal.h"
#include "utils/error.h"
#include "utils/string_utils.h"
#include "utils/structured_log.h"
#include "vectors/metadata_store.h"
#include "vectors/vector_store.h"

namespace nvecd::server {

namespace {

bool IsSnapshotProtectedWrite(CommandType type) {
  return type == CommandType::kEvent || type == CommandType::kVecset || type == CommandType::kVecdel ||
         type == CommandType::kMetaset;
}

bool IsSnapshotProtectedCommand(CommandType type) {
  return IsSnapshotProtectedWrite(type) || type == CommandType::kSim || type == CommandType::kSimv;
}

}  // namespace

RequestDispatcher::RequestDispatcher(HandlerContext& handler_ctx) : ctx_(handler_ctx) {}

std::string RequestDispatcher::Dispatch(const std::string& request, ConnectionContext& conn_ctx) {
  // Parse command. Pass the configured maximum top_k so the upper bound is
  // enforced at parse time (0 = no check when no config is wired).
  const uint32_t max_top_k = ctx_.config != nullptr ? ctx_.config->similarity.max_top_k : 0;
  auto cmd = ParseCommand(request, max_top_k);
  if (!cmd) {
    utils::LogCommandParseError(request, cmd.error().message(), 0);
    return FormatError(cmd.error().message());
  }

  // Handle AUTH command
  if (cmd->type == CommandType::kAuth) {
    return HandleAuth(*cmd, conn_ctx);
  }

  // Check authorization for non-read commands
  if (!ctx_.requirepass.empty()) {
    auto privilege = GetCommandPrivilege(cmd->type);
    if (privilege != CommandPrivilege::kRead && !conn_ctx.authenticated) {
      ctx_.stats.failed_commands++;
      return FormatError("NOAUTH Authentication required");
    }
  }

  if (IsSnapshotProtectedCommand(cmd->type) && ctx_.loading.load(std::memory_order_acquire)) {
    ctx_.stats.failed_commands++;
    return FormatError("LOADING Snapshot load in progress");
  }

  // Lock-mode snapshots set read_only before taking their store-lock barrier.
  // Reject a write before it can enter a store mutation path so the snapshot is
  // a true point-in-time image rather than a mix of pre/post-barrier updates.
  if (GetCommandPrivilege(cmd->type) != CommandPrivilege::kRead && ctx_.read_only.load(std::memory_order_acquire)) {
    ctx_.stats.failed_commands++;
    return FormatError("READONLY Snapshot in progress");
  }

  // Keep the shared gate for the full store-mutation and WAL append sequence.
  // A lock-mode snapshot sets read_only first, then takes this gate exclusively
  // to drain any writer that already passed the initial flag check. Rechecking
  // after acquisition closes the check-then-mutate race at the boundary.
  std::shared_lock<std::shared_mutex> snapshot_write_guard;
  if (IsSnapshotProtectedCommand(cmd->type) && ctx_.snapshot_write_gate != nullptr) {
    snapshot_write_guard = std::shared_lock(*ctx_.snapshot_write_gate);
    if (ctx_.loading.load(std::memory_order_acquire)) {
      ctx_.stats.failed_commands++;
      return FormatError("LOADING Snapshot load in progress");
    }
    if (ctx_.read_only.load(std::memory_order_acquire)) {
      if (IsSnapshotProtectedWrite(cmd->type)) {
        ctx_.stats.failed_commands++;
        return FormatError("READONLY Snapshot in progress");
      }
    }
  }

  // Keep the same command-level critical section through the WAL append. The
  // WAL is replayed in sequence order, so allowing another writer to append a
  // dependent METASET before this VECSET is durable corrupts recovery.
  std::unique_lock<std::mutex> write_serialization_guard;
  if (IsSnapshotProtectedWrite(cmd->type) && ctx_.write_serialization_gate != nullptr) {
    write_serialization_guard = std::unique_lock(*ctx_.write_serialization_gate);
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

    case CommandType::kVecdel:
      result = HandleVecdel(*cmd);
      break;

    case CommandType::kMetaset:
      result = HandleMetaset(*cmd);
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

    case CommandType::kDumpStatus:
      ctx_.stats.dump_commands++;
      result = HandleDumpStatus();
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

    case CommandType::kAuth:
      // Handled above before the switch; should not reach here
      break;

    case CommandType::kUnknown:
      result = utils::MakeUnexpected(utils::MakeError(utils::ErrorCode::kCommandUnknown, "Unknown command"));
      break;
  }

  // Handle result — always increment total_commands
  ctx_.stats.total_commands++;

  if (!result) {
    ctx_.stats.failed_commands++;
    return FormatError(result.error().message());
  }

  return *result;
}

//
// Handler implementations
//

utils::Expected<std::string, utils::Error> RequestDispatcher::HandleEvent(const Command& cmd) const {
  if (ctx_.event_store == nullptr) {
    return utils::MakeUnexpected(utils::MakeError(utils::ErrorCode::kInternalError, "EventStore not initialized"));
  }

  auto prepared = ctx_.event_store->PrepareEvent(cmd.ctx, cmd.id, cmd.score, cmd.event_type, cmd.timestamp.value_or(0));
  if (!prepared) {
    return utils::MakeUnexpected(prepared.error());
  }

  if (prepared->deduped) {
    // Apply the no-op through the normal path so observability counters remain
    // accurate, but do not write a WAL record for a deduplicated event.
    auto duplicate =
        ctx_.event_store->AddEventAndGetPrior(cmd.ctx, cmd.id, cmd.score, cmd.event_type, prepared->event.timestamp);
    if (!duplicate || !duplicate->deduped) {
      ctx_.read_only.store(true, std::memory_order_release);
      return utils::MakeUnexpected(
          utils::MakeError(utils::ErrorCode::kInternalError, "Event dedup state changed during acceptance"));
    }
    return FormatOK("EVENT");
  }

  Command wal_cmd = cmd;
  wal_cmd.score = prepared->event.score;
  wal_cmd.event_type = prepared->event.type;
  wal_cmd.timestamp = prepared->event.timestamp;
  auto wal_result = AppendToWal(wal_cmd);
  if (!wal_result) {
    return utils::MakeUnexpected(wal_result.error());
  }

  // Apply only after the WAL record has been accepted. The dispatcher-wide
  // write gate keeps the preview and commit free from competing mutations.
  auto result = ctx_.event_store->AddEventAndGetPrior(cmd.ctx, cmd.id, prepared->event.score, prepared->event.type,
                                                      prepared->event.timestamp);
  if (!result || result->deduped) {
    ctx_.read_only.store(true, std::memory_order_release);
    return utils::MakeUnexpected(
        utils::MakeError(utils::ErrorCode::kInternalError, "Event apply diverged after WAL acceptance"));
  }

  // Update co-occurrence index incrementally (only new pairs, once each).
  if (ctx_.co_index != nullptr) {
    events::CoOccurrenceIndex::IngestOptions options;
    options.temporal_enabled = ctx_.config->events.temporal_cooccurrence;
    options.half_life_sec = ctx_.config->events.temporal_half_life_sec;
    options.negative_signals = ctx_.config->events.negative_signals;
    options.negative_weight = ctx_.config->events.negative_weight;
    ctx_.co_index->ApplyIngestedEvent(cmd.ctx, result->prior_events, result->stored_event, options);
  }

  // Selective cache invalidation for mutated item
  auto* cache_ptr = ctx_.cache.load(std::memory_order_acquire);
  if (cache_ptr != nullptr) {
    cache_ptr->InvalidateByItemId(cmd.id);
  }

  return FormatOK("EVENT");
}

utils::Expected<std::string, utils::Error> RequestDispatcher::HandleVecset(const Command& cmd) const {
  if (ctx_.vector_store == nullptr) {
    return utils::MakeUnexpected(utils::MakeError(utils::ErrorCode::kInternalError, "VectorStore not initialized"));
  }

  auto validation = ctx_.vector_store->ValidateVector(cmd.id, cmd.vector);
  if (!validation) {
    return utils::MakeUnexpected(validation.error());
  }

  // WAL-before-apply: a failed durability acceptance must leave every live
  // store, ANN generation, and cache unchanged.
  auto wal_result = AppendToWal(cmd);
  if (!wal_result) {
    return utils::MakeUnexpected(wal_result.error());
  }

  auto result = ctx_.vector_store->SetVector(cmd.id, cmd.vector);
  if (!result) {
    ctx_.read_only.store(true, std::memory_order_release);
    return utils::MakeUnexpected(result.error());
  }
  if (cmd.metadata.has_value() && ctx_.metadata_store != nullptr) {
    ctx_.metadata_store->Set(cmd.id, *cmd.metadata);
  }

  // Notify IVF index of the new/updated vector.
  // Copy the vector data under a brief read lock, then notify without holding any lock.
  // This avoids recursive shared_mutex acquisition (undefined behavior in C++17).
  if (ctx_.similarity_engine != nullptr) {
    // Resolve the index and copy the vector data atomically under a single
    // snapshot (read lock). Looking the index up separately from the copy
    // would race with a concurrent defragment that re-indexes slots.
    std::optional<size_t> compact_idx;
    std::vector<float> vec_copy;
    {
      auto snap = ctx_.vector_store->GetCompactSnapshot();
      if (!snap.Empty()) {
        auto idx_it = snap.id_to_idx->find(cmd.id);
        if (idx_it != snap.id_to_idx->end()) {
          compact_idx = idx_it->second;
          const float* vec_ptr = snap.matrix + idx_it->second * snap.dim;
          vec_copy.assign(vec_ptr, vec_ptr + snap.dim);
        }
      }
    }
    if (compact_idx.has_value()) {
      ctx_.similarity_engine->NotifyVectorAdded(compact_idx.value(), vec_copy.data());
    }
  }

  // Bump the vector-store generation so SIM/SIMV cache keys derived from the
  // vector store change. The per-item reverse index only evicts entries that
  // already reference an existing ID, so a brand-new item would otherwise be a
  // no-op and stale cached results could omit it. The generation participates
  // in both SIM and SIMV keys, invalidating that space on any vector mutation.
  ctx_.vector_generation.fetch_add(1, std::memory_order_acq_rel);
  if (cmd.metadata.has_value()) {
    ctx_.metadata_generation.fetch_add(1, std::memory_order_acq_rel);
  }

  // Selective cache invalidation for mutated item
  auto* cache_ptr = ctx_.cache.load(std::memory_order_acquire);
  if (cache_ptr != nullptr) {
    cache_ptr->InvalidateByItemId(cmd.id);
    if (cmd.metadata.has_value()) {
      cache_ptr->Clear();
    }
  }

  return FormatOK("VECSET");
}

utils::Expected<std::string, utils::Error> RequestDispatcher::HandleVecdel(const Command& cmd) const {
  if (ctx_.vector_store == nullptr) {
    return utils::MakeUnexpected(utils::MakeError(utils::ErrorCode::kInternalError, "VectorStore not initialized"));
  }

  // Capture the compact index before deletion. The store may defragment during
  // DeleteVector(), invalidating every later compact index, so ANN cleanup must
  // happen before the mutation and a full rebuild follows it.
  const auto compact_index = ctx_.vector_store->GetCompactIndex(cmd.id);
  if (!compact_index.has_value()) {
    return utils::MakeUnexpected(utils::MakeError(utils::ErrorCode::kVectorNotFound, "Vector not found: " + cmd.id));
  }
  auto wal_result = AppendToWal(cmd);
  if (!wal_result) {
    return utils::MakeUnexpected(wal_result.error());
  }
  if (!ctx_.vector_store->DeleteVector(cmd.id)) {
    ctx_.read_only.store(true, std::memory_order_release);
    return utils::MakeUnexpected(utils::MakeError(utils::ErrorCode::kVectorNotFound, "Vector not found: " + cmd.id));
  }
  // RebuildAnnFromStore labels entries by compact index. Compact the store
  // unconditionally after a public delete so the rebuilt index never sees a
  // tombstoned row or a shifted label.
  ctx_.vector_store->Defragment();

  if (ctx_.metadata_store != nullptr) {
    ctx_.metadata_store->Delete(cmd.id);
  }
  if (ctx_.similarity_engine != nullptr) {
    ctx_.similarity_engine->NotifyVectorRemoved(*compact_index);
    // Deletion can trigger VectorStore defragmentation and re-key every live
    // compact index. Rebuilding is required for HNSW/IVF labels to remain
    // associated with the correct external IDs.
    ctx_.similarity_engine->RebuildAnnFromStore();
  }

  ctx_.vector_generation.fetch_add(1, std::memory_order_acq_rel);
  ctx_.metadata_generation.fetch_add(1, std::memory_order_acq_rel);
  auto* cache_ptr = ctx_.cache.load(std::memory_order_acquire);
  if (cache_ptr != nullptr) {
    cache_ptr->InvalidateByItemId(cmd.id);
  }

  return FormatOK("VECDEL");
}

utils::Expected<std::string, utils::Error> RequestDispatcher::HandleMetaset(const Command& cmd) const {
  if (ctx_.vector_store == nullptr) {
    return utils::MakeUnexpected(utils::MakeError(utils::ErrorCode::kInternalError, "VectorStore not initialized"));
  }
  if (ctx_.metadata_store == nullptr) {
    return utils::MakeUnexpected(utils::MakeError(utils::ErrorCode::kInternalError, "MetadataStore not initialized"));
  }

  // Validate that the target vector exists; METASET must fail for unknown IDs.
  if (!ctx_.vector_store->GetCompactIndex(cmd.id).has_value()) {
    return utils::MakeUnexpected(
        utils::MakeError(utils::ErrorCode::kVectorNotFound, "Vector not found for metadata: " + cmd.id));
  }

  vectors::Metadata metadata;
  if (cmd.metadata.has_value()) {
    metadata = *cmd.metadata;
  } else {
    auto parsed = ParseSimpleFilter(cmd.filter_expr);
    if (!parsed) {
      return utils::MakeUnexpected(parsed.error());
    }
    for (const auto& condition : parsed->conditions) {
      if (condition.field.empty()) {
        return utils::MakeUnexpected(
            utils::MakeError(utils::ErrorCode::kCommandInvalidArgument, "Metadata key must not be empty"));
      }
      metadata[condition.field] = condition.value;
    }
  }
  auto wal_result = AppendToWal(cmd);
  if (!wal_result) {
    return utils::MakeUnexpected(wal_result.error());
  }
  ctx_.metadata_store->Set(cmd.id, std::move(metadata));
  ctx_.metadata_generation.fetch_add(1, std::memory_order_acq_rel);

  auto* cache_ptr = ctx_.cache.load(std::memory_order_acquire);
  if (cache_ptr != nullptr) {
    cache_ptr->Clear();
  }

  return FormatOK("METASET");
}

utils::Expected<std::string, utils::Error> RequestDispatcher::HandleSim(const Command& cmd,
                                                                        ConnectionContext& conn_ctx) const {
  if (ctx_.similarity_engine == nullptr) {
    return utils::MakeUnexpected(
        utils::MakeError(utils::ErrorCode::kInternalError, "SimilarityEngine not initialized"));
  }

  // Cache lookup
  auto* cache_ptr = ctx_.cache.load(std::memory_order_acquire);
  cache::CacheKey cache_key;
  bool cache_enabled = (cache_ptr != nullptr && cache_ptr->IsEnabled());
  auto search_type = cache::SearchType::kItemSearch;
  uint64_t captured_cooccurrence_generation = 0;
  uint64_t captured_vector_generation = 0;
  uint64_t captured_metadata_generation = 0;
  uint64_t captured_dataset_generation = 0;

  // Parse filter expression if provided
  vectors::MetadataFilter filter;
  if (!cmd.filter_expr.empty()) {
    auto filter_result = ParseSimpleFilter(cmd.filter_expr);
    if (!filter_result) {
      return utils::MakeUnexpected(filter_result.error());
    }
    filter = std::move(*filter_result);
    search_type = cache::SearchType::kFilteredSearch;
  }

  if (cache_enabled) {
    captured_cooccurrence_generation = ctx_.co_index != nullptr ? ctx_.co_index->GetGeneration() : 0;
    captured_vector_generation = ctx_.vector_generation.load(std::memory_order_acquire);
    captured_metadata_generation = ctx_.metadata_generation.load(std::memory_order_acquire);
    captured_dataset_generation = ctx_.dataset_generation.load(std::memory_order_acquire);
    cache_key = cache::GenerateSimCacheKey({cmd.id, cmd.top_k, cmd.mode, cmd.adaptive, captured_cooccurrence_generation,
                                            captured_vector_generation, cmd.filter_expr, captured_metadata_generation,
                                            captured_dataset_generation});
    auto cached = cache_ptr->Lookup(cache_key, search_type);
    if (cached.has_value()) {
      auto pairs = ApplyMinScore(*cached, cmd.min_score);
      std::string response = FormatSimResults(pairs, static_cast<int>(pairs.size()));
      if (conn_ctx.debug_mode) {
        // Cache hit: timing reflects the lookup only and the candidate count
        // equals the cached result count.
        response += handlers::FormatSimDebugBlock(cmd.mode, 0.0, static_cast<int>(cached->size()),
                                                  static_cast<int>(pairs.size()));
      }
      return response;
    }
  }

  auto start = std::chrono::steady_clock::now();

  // Select search method based on mode.
  utils::Expected<std::vector<similarity::SimilarityResult>, utils::Error> result;
  if (cmd.mode == "events") {
    result = ctx_.similarity_engine->SearchByIdEvents(cmd.id, cmd.top_k, filter);
  } else if (cmd.mode == "vectors") {
    result = ctx_.similarity_engine->SearchByIdVectors(cmd.id, cmd.top_k, filter);
  } else {  // fusion (default)
    result = ctx_.similarity_engine->SearchByIdFusion(cmd.id, cmd.top_k, cmd.adaptive, filter);
  }

  if (!result) {
    return utils::MakeUnexpected(result.error());
  }
  if (cmd.mode != "events") {
    // Non-events modes already apply the filter inside the engine; this second
    // pass enforces it consistently for results that bypassed engine filtering.
    *result = ApplyMetadataFilter(*result, ctx_.metadata_store, filter);
  }

  auto elapsed = std::chrono::steady_clock::now() - start;
  double elapsed_ms = std::chrono::duration<double, std::milli>(elapsed).count();

  // Cache store
  if (cache_enabled) {
    std::unique_lock<std::mutex> generation_guard;
    if (ctx_.write_serialization_gate != nullptr) {
      generation_guard = std::unique_lock(*ctx_.write_serialization_gate);
    }
    if (cache_ptr == ctx_.cache.load(std::memory_order_acquire) && cache_ptr->IsEnabled() &&
        captured_cooccurrence_generation == (ctx_.co_index != nullptr ? ctx_.co_index->GetGeneration() : 0) &&
        captured_vector_generation == ctx_.vector_generation.load(std::memory_order_acquire) &&
        captured_metadata_generation == ctx_.metadata_generation.load(std::memory_order_acquire) &&
        captured_dataset_generation == ctx_.dataset_generation.load(std::memory_order_acquire)) {
      std::vector<std::string> item_ids;
      item_ids.reserve(result->size() + 1);
      item_ids.push_back(cmd.id);  // Query ID itself
      for (const auto& item : *result) {
        item_ids.push_back(item.item_id);
      }
      cache_ptr->InsertAndRegister(cache_key, *result, item_ids, elapsed_ms, search_type);
    }
  }

  // Apply min_score filter and convert to pair<string, float>
  auto pairs = ApplyMinScore(*result, cmd.min_score);

  std::string response = FormatSimResults(pairs, static_cast<int>(pairs.size()));
  if (conn_ctx.debug_mode) {
    response += handlers::FormatSimDebugBlock(cmd.mode, elapsed_ms, static_cast<int>(result->size()),
                                              static_cast<int>(pairs.size()));
  }
  return response;
}

utils::Expected<std::string, utils::Error> RequestDispatcher::HandleSimv(const Command& cmd,
                                                                         ConnectionContext& conn_ctx) const {
  if (ctx_.similarity_engine == nullptr) {
    return utils::MakeUnexpected(
        utils::MakeError(utils::ErrorCode::kInternalError, "SimilarityEngine not initialized"));
  }

  // Cache lookup
  auto* cache_ptr = ctx_.cache.load(std::memory_order_acquire);
  cache::CacheKey cache_key;
  bool cache_enabled = (cache_ptr != nullptr && cache_ptr->IsEnabled());
  auto search_type = cache::SearchType::kVectorSearch;
  uint64_t captured_vector_generation = 0;
  uint64_t captured_metadata_generation = 0;
  uint64_t captured_dataset_generation = 0;

  // Parse filter expression if provided
  vectors::MetadataFilter filter;
  if (!cmd.filter_expr.empty()) {
    auto filter_result = ParseSimpleFilter(cmd.filter_expr);
    if (!filter_result) {
      return utils::MakeUnexpected(filter_result.error());
    }
    filter = std::move(*filter_result);
    search_type = cache::SearchType::kFilteredSearch;
  }

  if (cache_enabled) {
    captured_vector_generation = ctx_.vector_generation.load(std::memory_order_acquire);
    captured_metadata_generation = ctx_.metadata_generation.load(std::memory_order_acquire);
    captured_dataset_generation = ctx_.dataset_generation.load(std::memory_order_acquire);
    cache_key = cache::GenerateSimvCacheKey({cmd.vector, cmd.top_k, captured_vector_generation, cmd.filter_expr,
                                             captured_metadata_generation, captured_dataset_generation});
    auto cached = cache_ptr->Lookup(cache_key, search_type);
    if (cached.has_value()) {
      auto pairs = ApplyMinScore(*cached, cmd.min_score);
      std::string response = FormatSimResults(pairs, static_cast<int>(pairs.size()));
      if (conn_ctx.debug_mode) {
        response += handlers::FormatSimDebugBlock("vector", 0.0, static_cast<int>(cached->size()),
                                                  static_cast<int>(pairs.size()));
      }
      return response;
    }
  }

  auto start = std::chrono::steady_clock::now();

  auto result = ctx_.similarity_engine->SearchByVector(cmd.vector, cmd.top_k, filter);
  if (!result) {
    return utils::MakeUnexpected(result.error());
  }
  *result = ApplyMetadataFilter(*result, ctx_.metadata_store, filter);

  auto elapsed = std::chrono::steady_clock::now() - start;
  double elapsed_ms = std::chrono::duration<double, std::milli>(elapsed).count();

  // Cache store
  if (cache_enabled) {
    std::unique_lock<std::mutex> generation_guard;
    if (ctx_.write_serialization_gate != nullptr) {
      generation_guard = std::unique_lock(*ctx_.write_serialization_gate);
    }
    if (cache_ptr == ctx_.cache.load(std::memory_order_acquire) && cache_ptr->IsEnabled() &&
        captured_vector_generation == ctx_.vector_generation.load(std::memory_order_acquire) &&
        captured_metadata_generation == ctx_.metadata_generation.load(std::memory_order_acquire) &&
        captured_dataset_generation == ctx_.dataset_generation.load(std::memory_order_acquire)) {
      std::vector<std::string> item_ids;
      item_ids.reserve(result->size());
      for (const auto& item : *result) {
        item_ids.push_back(item.item_id);
      }
      cache_ptr->InsertAndRegister(cache_key, *result, item_ids, elapsed_ms, search_type);
    }
  }

  // Apply min_score filter and convert to pair<string, float>
  auto pairs = ApplyMinScore(*result, cmd.min_score);

  std::string response = FormatSimResults(pairs, static_cast<int>(pairs.size()));
  if (conn_ctx.debug_mode) {
    response += handlers::FormatSimDebugBlock("vector", elapsed_ms, static_cast<int>(result->size()),
                                              static_cast<int>(pairs.size()));
  }
  return response;
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

utils::Expected<std::string, utils::Error> RequestDispatcher::HandleDumpStatus() {
  return handlers::HandleDumpStatus(ctx_);
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
    // Use the shared fixed-precision policy so scores render identically on the
    // TCP and HTTP surfaces.
    oss << id << " " << FormatScore(score) << "\r\n";
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

//
// Auth handler
//

std::string RequestDispatcher::HandleAuth(const Command& cmd, ConnectionContext& conn_ctx) {
  if (ctx_.requirepass.empty()) {
    // No password configured - auth not needed
    return "+OK (no password required)\r\n";
  }

  if (utils::ConstantTimeEquals(cmd.variable_value, ctx_.requirepass)) {
    conn_ctx.authenticated = true;
    return "+OK\r\n";
  }

  ctx_.stats.failed_commands++;
  return FormatError("ERR invalid password");
}

//
// Write-Ahead Log integration
//

utils::Expected<void, utils::Error> RequestDispatcher::AppendToWal(const Command& cmd) const {
  if (ctx_.wal == nullptr) {
    return {};
  }

  // Operators can opt out of retaining vector payloads in the WAL when
  // snapshots are their chosen vector durability boundary. Keep all other
  // mutations in the log so event and metadata recovery semantics are intact.
  if (cmd.type == CommandType::kVecset && ctx_.config != nullptr && !ctx_.config->wal.include_vectors) {
    return {};
  }

  std::vector<uint8_t> payload = EncodeCommand(cmd);
  auto appended = ctx_.wal->Append(WalOpForCommand(cmd), payload.data(), payload.size());
  if (!appended) {
    ctx_.read_only.store(true, std::memory_order_release);
    utils::StructuredLog()
        .Event("wal_append_failed")
        .Field("command", CommandTypeToString(cmd.type))
        .Field("error", appended.error().message())
        .Warn();
    return utils::MakeUnexpected(appended.error());
  }
  return {};
}

utils::Expected<void, utils::Error> RequestDispatcher::ReplayRecord(const storage::WalRecord& record) {
  if (record.op == storage::WalOpType::kCoOccurrenceMaintenance) {
    constexpr size_t kMaintenancePayloadSize = sizeof(double) + sizeof(uint8_t);
    if (record.payload.size() != kMaintenancePayloadSize) {
      return utils::MakeUnexpected(
          utils::MakeError(utils::ErrorCode::kWalCorrupted, "Invalid co-occurrence maintenance WAL payload size"));
    }
    double alpha = 0.0;
    std::memcpy(&alpha, record.payload.data(), sizeof(alpha));
    const uint8_t prune = record.payload[sizeof(alpha)];
    if (!std::isfinite(alpha) || alpha <= 0.0 || alpha > 1.0 || prune > 1) {
      return utils::MakeUnexpected(
          utils::MakeError(utils::ErrorCode::kWalCorrupted, "Invalid co-occurrence maintenance WAL payload"));
    }
    ctx_.co_index->ApplyDecay(alpha);
    if (prune != 0) {
      ctx_.co_index->Prune();
    }
    return {};
  }

  auto decoded = DecodeWalRecord(record);
  if (!decoded) {
    utils::StructuredLog()
        .Event("wal_replay_decode_failed")
        .Field("sequence", static_cast<int64_t>(record.sequence))
        .Field("error", decoded.error().message())
        .Warn();
    return utils::MakeUnexpected(decoded.error());
  }

  // Re-apply via the matching write handler. ctx_.wal is null during replay, so
  // these handlers will not re-append the record to the WAL.
  utils::Expected<std::string, utils::Error> applied =
      utils::MakeUnexpected(utils::MakeError(utils::ErrorCode::kCommandUnknown, "Unsupported WAL command"));
  switch (decoded->type) {
    case CommandType::kEvent:
      applied = HandleEvent(*decoded);
      break;
    case CommandType::kVecset:
      applied = HandleVecset(*decoded);
      break;
    case CommandType::kVecdel:
      applied = HandleVecdel(*decoded);
      break;
    case CommandType::kMetaset:
      applied = HandleMetaset(*decoded);
      break;
    default:
      break;
  }

  if (!applied) {
    utils::StructuredLog()
        .Event("wal_replay_apply_failed")
        .Field("sequence", static_cast<int64_t>(record.sequence))
        .Field("error", applied.error().message())
        .Warn();
    return utils::MakeUnexpected(applied.error());
  }
  return {};
}

}  // namespace nvecd::server
