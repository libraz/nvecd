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

/**
 * @brief Append a write command to the WAL
 *
 * A failed append is propagated so no caller acknowledges a durable write the
 * log could not accept.
 *
 * @param ctx Handler context (ctx.wal is null during replay, which skips the append)
 * @param cmd Effective write command to persist (timestamps already resolved)
 */
utils::Expected<void, utils::Error> AppendToWal(HandlerContext& ctx, const Command& cmd) {
  if (ctx.wal == nullptr) {
    return {};
  }

  // Operators can opt out of retaining vector payloads in the WAL when
  // snapshots are their chosen vector durability boundary. Keep all other
  // mutations in the log so event and metadata recovery semantics are intact.
  if (cmd.type == CommandType::kVecset && ctx.config != nullptr && !ctx.config->wal.include_vectors) {
    return {};
  }

  std::vector<uint8_t> payload = EncodeCommand(cmd);
  auto appended = ctx.wal->Append(WalOpForCommand(cmd), payload.data(), payload.size());
  if (!appended) {
    ctx.read_only.store(true, std::memory_order_release);
    utils::StructuredLog()
        .Event("wal_append_failed")
        .Field("command", CommandTypeToString(cmd.type))
        .Field("error", appended.error().message())
        .Warn();
    return utils::MakeUnexpected(appended.error());
  }
  return {};
}

utils::Expected<WriteOutcome, utils::Error> ApplyEvent(HandlerContext& ctx, const Command& cmd) {
  if (ctx.event_store == nullptr) {
    return utils::MakeUnexpected(utils::MakeError(utils::ErrorCode::kInternalError, "EventStore not initialized"));
  }

  auto prepared = ctx.event_store->PrepareEvent(cmd.ctx, cmd.id, cmd.score, cmd.event_type, cmd.timestamp.value_or(0));
  if (!prepared) {
    return utils::MakeUnexpected(prepared.error());
  }

  if (prepared->deduped) {
    // Apply the no-op through the normal path so observability counters remain
    // accurate, but do not write a WAL record for a deduplicated event.
    auto duplicate =
        ctx.event_store->AddEventAndGetPrior(cmd.ctx, cmd.id, cmd.score, cmd.event_type, prepared->event.timestamp);
    if (!duplicate || !duplicate->deduped) {
      ctx.read_only.store(true, std::memory_order_release);
      return utils::MakeUnexpected(
          utils::MakeError(utils::ErrorCode::kInternalError, "Event dedup state changed during acceptance"));
    }
    return WriteOutcome{CommandType::kEvent, /*deduplicated=*/true, /*dimension=*/0};
  }

  Command wal_cmd = cmd;
  wal_cmd.score = prepared->event.score;
  wal_cmd.event_type = prepared->event.type;
  wal_cmd.timestamp = prepared->event.timestamp;
  auto wal_result = AppendToWal(ctx, wal_cmd);
  if (!wal_result) {
    return utils::MakeUnexpected(wal_result.error());
  }

  // Apply only after the WAL record has been accepted. The write gate keeps the
  // preview and commit free from competing mutations.
  auto result = ctx.event_store->AddEventAndGetPrior(cmd.ctx, cmd.id, prepared->event.score, prepared->event.type,
                                                     prepared->event.timestamp);
  if (!result || result->deduped) {
    ctx.read_only.store(true, std::memory_order_release);
    return utils::MakeUnexpected(
        utils::MakeError(utils::ErrorCode::kInternalError, "Event apply diverged after WAL acceptance"));
  }

  // Update co-occurrence index incrementally (only new pairs, once each).
  if (ctx.co_index != nullptr) {
    events::CoOccurrenceIndex::IngestOptions options;
    if (ctx.config != nullptr) {
      options.temporal_enabled = ctx.config->events.temporal_cooccurrence;
      options.half_life_sec = ctx.config->events.temporal_half_life_sec;
      options.negative_signals = ctx.config->events.negative_signals;
      options.negative_weight = ctx.config->events.negative_weight;
    }
    ctx.co_index->ApplyIngestedEvent(cmd.ctx, result->prior_events, result->stored_event, options);
  }

  // Selective cache invalidation for mutated item
  auto* cache_ptr = ctx.cache.load(std::memory_order_acquire);
  if (cache_ptr != nullptr) {
    cache_ptr->InvalidateByItemId(cmd.id);
  }

  return WriteOutcome{CommandType::kEvent, /*deduplicated=*/false, /*dimension=*/0};
}

utils::Expected<WriteOutcome, utils::Error> ApplyVecset(HandlerContext& ctx, const Command& cmd) {
  if (ctx.vector_store == nullptr) {
    return utils::MakeUnexpected(utils::MakeError(utils::ErrorCode::kInternalError, "VectorStore not initialized"));
  }

  auto validation = ctx.vector_store->ValidateVector(cmd.id, cmd.vector);
  if (!validation) {
    return utils::MakeUnexpected(validation.error());
  }

  // WAL-before-apply: a failed durability acceptance must leave every live
  // store, ANN generation, and cache unchanged.
  auto wal_result = AppendToWal(ctx, cmd);
  if (!wal_result) {
    return utils::MakeUnexpected(wal_result.error());
  }

  auto result = ctx.vector_store->SetVector(cmd.id, cmd.vector);
  if (!result) {
    ctx.read_only.store(true, std::memory_order_release);
    return utils::MakeUnexpected(result.error());
  }
  if (cmd.metadata.has_value() && ctx.metadata_store != nullptr) {
    ctx.metadata_store->Set(cmd.id, *cmd.metadata);
  }

  // Notify IVF index of the new/updated vector.
  // Copy the vector data under a brief read lock, then notify without holding any lock.
  // This avoids recursive shared_mutex acquisition (undefined behavior in C++17).
  if (ctx.similarity_engine != nullptr) {
    // Resolve the index and copy the vector data atomically under a single
    // snapshot (read lock). Looking the index up separately from the copy
    // would race with a concurrent defragment that re-indexes slots.
    std::optional<size_t> compact_idx;
    std::vector<float> vec_copy;
    {
      auto snap = ctx.vector_store->GetCompactSnapshot();
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
      ctx.similarity_engine->NotifyVectorAdded(compact_idx.value(), vec_copy.data());
    }
  }

  // Bump the vector-store generation so SIM/SIMV cache keys derived from the
  // vector store change. The per-item reverse index only evicts entries that
  // already reference an existing ID, so a brand-new item would otherwise be a
  // no-op and stale cached results could omit it. The generation participates
  // in both SIM and SIMV keys, invalidating that space on any vector mutation.
  ctx.vector_generation.fetch_add(1, std::memory_order_acq_rel);
  if (cmd.metadata.has_value()) {
    ctx.metadata_generation.fetch_add(1, std::memory_order_acq_rel);
  }

  // Selective cache invalidation for mutated item
  auto* cache_ptr = ctx.cache.load(std::memory_order_acquire);
  if (cache_ptr != nullptr) {
    cache_ptr->InvalidateByItemId(cmd.id);
    if (cmd.metadata.has_value()) {
      cache_ptr->Clear();
    }
  }

  return WriteOutcome{CommandType::kVecset, /*deduplicated=*/false, cmd.vector.size()};
}

utils::Expected<WriteOutcome, utils::Error> ApplyVecdel(HandlerContext& ctx, const Command& cmd) {
  if (ctx.vector_store == nullptr) {
    return utils::MakeUnexpected(utils::MakeError(utils::ErrorCode::kInternalError, "VectorStore not initialized"));
  }

  // Capture the compact index before deletion. The store may compact during
  // DeleteVector(), invalidating every later compact index, so the ANN cleanup
  // decision is made from row counts observed around the mutation.
  const auto compact_index = ctx.vector_store->GetCompactIndex(cmd.id);
  if (!compact_index.has_value()) {
    return utils::MakeUnexpected(utils::MakeError(utils::ErrorCode::kVectorNotFound, "Vector not found: " + cmd.id));
  }
  auto wal_result = AppendToWal(ctx, cmd);
  if (!wal_result) {
    return utils::MakeUnexpected(wal_result.error());
  }
  const size_t rows_before = ctx.vector_store->GetCompactCount();
  if (!ctx.vector_store->DeleteVector(cmd.id)) {
    ctx.read_only.store(true, std::memory_order_release);
    return utils::MakeUnexpected(utils::MakeError(utils::ErrorCode::kVectorNotFound, "Vector not found: " + cmd.id));
  }

  if (ctx.metadata_store != nullptr) {
    ctx.metadata_store->Delete(cmd.id);
  }
  if (ctx.similarity_engine != nullptr) {
    // The store compacts itself once its own fragmentation threshold is
    // reached; a delete that left a tombstone in place has not moved any other
    // row, so removing the single label is enough. Only a compaction re-keys
    // every live compact index and therefore requires a full rebuild.
    if (ctx.vector_store->GetCompactCount() < rows_before) {
      ctx.similarity_engine->RebuildAnnFromStore();
    } else {
      ctx.similarity_engine->NotifyVectorRemoved(*compact_index);
    }
  }

  ctx.vector_generation.fetch_add(1, std::memory_order_acq_rel);
  ctx.metadata_generation.fetch_add(1, std::memory_order_acq_rel);
  auto* cache_ptr = ctx.cache.load(std::memory_order_acquire);
  if (cache_ptr != nullptr) {
    cache_ptr->InvalidateByItemId(cmd.id);
  }

  return WriteOutcome{CommandType::kVecdel, /*deduplicated=*/false, /*dimension=*/0};
}

utils::Expected<WriteOutcome, utils::Error> ApplyMetaset(HandlerContext& ctx, const Command& cmd) {
  if (ctx.vector_store == nullptr) {
    return utils::MakeUnexpected(utils::MakeError(utils::ErrorCode::kInternalError, "VectorStore not initialized"));
  }
  if (ctx.metadata_store == nullptr) {
    return utils::MakeUnexpected(utils::MakeError(utils::ErrorCode::kInternalError, "MetadataStore not initialized"));
  }

  // Validate that the target vector exists; METASET must fail for unknown IDs.
  if (!ctx.vector_store->GetCompactIndex(cmd.id).has_value()) {
    return utils::MakeUnexpected(
        utils::MakeError(utils::ErrorCode::kVectorNotFound, "Vector not found for metadata: " + cmd.id));
  }

  // Typed metadata arrives from the JSON surface and from WAL replay; the TCP
  // text protocol carries the same pairs as a filter expression.
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
  auto wal_result = AppendToWal(ctx, cmd);
  if (!wal_result) {
    return utils::MakeUnexpected(wal_result.error());
  }
  ctx.metadata_store->Set(cmd.id, std::move(metadata));
  ctx.metadata_generation.fetch_add(1, std::memory_order_acq_rel);

  // Metadata changes affect filtered results broadly, so the whole cache goes.
  auto* cache_ptr = ctx.cache.load(std::memory_order_acquire);
  if (cache_ptr != nullptr) {
    cache_ptr->Clear();
  }

  return WriteOutcome{CommandType::kMetaset, /*deduplicated=*/false, /*dimension=*/0};
}

}  // namespace

utils::Expected<WriteOutcome, utils::Error> ApplyWrite(HandlerContext& ctx, const Command& cmd) {
  // Keep the command-level critical section around the mutation and its WAL
  // append. The WAL is replayed in sequence order, so allowing another writer
  // to append a dependent METASET before this VECSET is durable corrupts
  // recovery.
  std::unique_lock<std::mutex> write_serialization_guard;
  if (ctx.write_serialization_gate != nullptr) {
    write_serialization_guard = std::unique_lock(*ctx.write_serialization_gate);
  }

  switch (cmd.type) {
    case CommandType::kEvent:
      return ApplyEvent(ctx, cmd);
    case CommandType::kVecset:
      return ApplyVecset(ctx, cmd);
    case CommandType::kVecdel:
      return ApplyVecdel(ctx, cmd);
    case CommandType::kMetaset:
      return ApplyMetaset(ctx, cmd);

    // Not writes: these never mutate a store and must not reach this function.
    case CommandType::kSim:
    case CommandType::kSimv:
    case CommandType::kInfo:
    case CommandType::kConfigHelp:
    case CommandType::kConfigShow:
    case CommandType::kConfigVerify:
    case CommandType::kDumpSave:
    case CommandType::kDumpLoad:
    case CommandType::kDumpVerify:
    case CommandType::kDumpInfo:
    case CommandType::kDumpStatus:
    case CommandType::kDebugOn:
    case CommandType::kDebugOff:
    case CommandType::kCacheStats:
    case CommandType::kCacheClear:
    case CommandType::kCacheEnable:
    case CommandType::kCacheDisable:
    case CommandType::kSet:
    case CommandType::kGet:
    case CommandType::kShowVariables:
    case CommandType::kAuth:
    case CommandType::kUnknown:
    case CommandType::kCount:
      break;
  }
  return utils::MakeUnexpected(utils::MakeError(utils::ErrorCode::kCommandUnknown,
                                                "Not a write command: " + std::string(CommandTypeToString(cmd.type))));
}

RequestDispatcher::RequestDispatcher(HandlerContext& handler_ctx) : ctx_(handler_ctx) {}

std::string RequestDispatcher::Dispatch(const std::string& request, ConnectionContext& conn_ctx) {
  // Parse command. Pass the configured maximum top_k so the upper bound is
  // enforced at parse time (0 = no check when no config is wired).
  const uint32_t max_top_k = ctx_.config != nullptr ? ctx_.config->similarity.max_top_k : 0;
  auto cmd = ParseCommand(request, max_top_k);
  if (!cmd) {
    // A rejected request is still a command the client sent: account it so the
    // failure ratio an operator alerts on cannot exceed one.
    CommandStatsScope parse_failure_scope(ctx_.stats, CommandType::kUnknown);
    utils::LogCommandParseError(request, cmd.error().message(), 0);
    return FormatError(cmd.error().message());
  }

  // Every exit below is accounted by this guard: total_commands on entry,
  // failed_commands on destruction unless the command succeeded.
  CommandStatsScope stats_scope(ctx_.stats, cmd->type);

  // AUTH is answered before the gate so a client can acquire the credential it
  // is about to be asked for.
  if (cmd->type == CommandType::kAuth) {
    auto auth_result = HandleAuth(*cmd, conn_ctx);
    if (!auth_result) {
      return FormatError(auth_result.error().message());
    }
    stats_scope.MarkSucceeded();
    return *auth_result;
  }

  // Check authorization for non-read commands
  if (!ctx_.requirepass.empty()) {
    auto privilege = GetCommandPrivilege(cmd->type);
    if (privilege != CommandPrivilege::kRead && !conn_ctx.authenticated) {
      return FormatError("NOAUTH Authentication required");
    }
  }

  if (IsSnapshotProtectedCommand(cmd->type) && ctx_.loading.load(std::memory_order_acquire)) {
    return FormatError("LOADING Snapshot load in progress");
  }

  // Lock-mode snapshots set read_only before taking their store-lock barrier.
  // Reject a write before it can enter a store mutation path so the snapshot is
  // a true point-in-time image rather than a mix of pre/post-barrier updates.
  if (GetCommandPrivilege(cmd->type) != CommandPrivilege::kRead && ctx_.read_only.load(std::memory_order_acquire)) {
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
      return FormatError("LOADING Snapshot load in progress");
    }
    if (ctx_.read_only.load(std::memory_order_acquire) && IsSnapshotProtectedWrite(cmd->type)) {
      return FormatError("READONLY Snapshot in progress");
    }
  }

  // Route to appropriate handler. Command counting is owned by stats_scope, so
  // no branch here touches a counter.
  utils::Expected<std::string, utils::Error> result =
      utils::MakeUnexpected(utils::MakeError(utils::ErrorCode::kCommandUnknown, "Unknown command type"));

  switch (cmd->type) {
    case CommandType::kEvent:
    case CommandType::kVecset:
    case CommandType::kVecdel:
    case CommandType::kMetaset:
      result = HandleWrite(*cmd);
      break;

    case CommandType::kSim:
      result = HandleSim(*cmd, conn_ctx);
      break;

    case CommandType::kSimv:
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

    case CommandType::kDumpStatus:
      result = HandleDumpStatus();
      break;

    case CommandType::kDebugOn:
      result = HandleDebugOn(conn_ctx);
      break;

    case CommandType::kDebugOff:
      result = HandleDebugOff(conn_ctx);
      break;

    case CommandType::kSet:
      result = HandleSet(*cmd);
      break;

    case CommandType::kGet:
      result = HandleGet(*cmd);
      break;

    case CommandType::kShowVariables:
      result = HandleShowVariables(*cmd);
      break;

    case CommandType::kCacheStats:
      result = handlers::HandleCacheStats(ctx_);
      break;

    case CommandType::kCacheClear:
      result = handlers::HandleCacheClear(ctx_);
      break;

    case CommandType::kCacheEnable:
      result = handlers::HandleCacheEnable(ctx_);
      break;

    case CommandType::kCacheDisable:
      result = handlers::HandleCacheDisable(ctx_);
      break;

    case CommandType::kAuth:
      // Handled above before the gate; unreachable here.
      break;

    case CommandType::kUnknown:
    case CommandType::kCount:
      result = utils::MakeUnexpected(utils::MakeError(utils::ErrorCode::kCommandUnknown, "Unknown command"));
      break;
  }

  if (!result) {
    return FormatError(result.error().message());
  }

  stats_scope.MarkSucceeded();
  return *result;
}

utils::Expected<std::string, utils::Error> RequestDispatcher::HandleWrite(const Command& cmd) const {
  auto outcome = ApplyWrite(ctx_, cmd);
  if (!outcome) {
    return utils::MakeUnexpected(outcome.error());
  }
  // The status word is written out literally rather than derived, so the
  // documented TCP vocabulary stays greppable in the source that emits it.
  // Exhaustiveness over CommandType is enforced inside ApplyWrite; anything
  // that reaches the fallback here is a write with no wire form, which is an
  // internal error rather than a silently formatted response.
  switch (outcome->type) {
    case CommandType::kEvent:
      return FormatOK("EVENT");
    case CommandType::kVecset:
      return FormatOK("VECSET");
    case CommandType::kVecdel:
      return FormatOK("VECDEL");
    case CommandType::kMetaset:
      return FormatOK("METASET");
    default:
      break;
  }
  return utils::MakeUnexpected(
      utils::MakeError(utils::ErrorCode::kInternalError, "Write produced an unformattable outcome"));
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
  return handlers::HandleConfigVerify(ctx_, cmd.path);
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

utils::Expected<std::string, utils::Error> RequestDispatcher::HandleAuth(const Command& cmd,
                                                                         ConnectionContext& conn_ctx) const {
  if (ctx_.requirepass.empty()) {
    // No password configured - auth not needed
    return std::string("+OK (no password required)\r\n");
  }

  if (utils::ConstantTimeEquals(cmd.variable_value, ctx_.requirepass)) {
    conn_ctx.authenticated = true;
    return std::string("+OK\r\n");
  }

  return utils::MakeUnexpected(utils::MakeError(utils::ErrorCode::kPermissionDenied, "ERR invalid password"));
}

//
// Write-Ahead Log integration
//

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

  // Re-apply through the same write choke point live traffic uses, so recovery
  // reconstructs exactly the state the original command produced. ctx_.wal is
  // null during replay, so the record is not appended a second time.
  auto applied = ApplyWrite(ctx_, *decoded);

  if (!applied) {
    if (IsIntendedReplayGap(record.op, applied.error().code())) {
      // The configuration that produced this log cannot restore this record's
      // subject. Skipping keeps recovery moving; aborting would make a server
      // configured with `wal.include_vectors: false` permanently unstartable
      // after any VECDEL or metadata write. Every skip is reported so the gap
      // is visible rather than silent, and the running total is reported by
      // INFO so the size of the gap survives log rotation.
      const uint64_t skipped_total = ctx_.stats.wal_replay_records_skipped.fetch_add(1) + 1;
      utils::StructuredLog()
          .Event("wal_replay_record_skipped")
          .Field("sequence", static_cast<int64_t>(record.sequence))
          .Field("op", static_cast<int64_t>(static_cast<uint8_t>(record.op)))
          .Field("item_id", decoded->id)
          .Field("skipped_total", static_cast<int64_t>(skipped_total))
          .Field("reason", applied.error().message())
          .Warn();
      return {};
    }
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
