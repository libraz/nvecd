/**
 * @file request_dispatcher.cpp
 * @brief Implementation of RequestDispatcher
 *
 * Reference: ../mygram-db/src/server/request_dispatcher.cpp
 * Reusability: 75% (similar dispatch pattern, different handlers)
 */

#include <spdlog/spdlog.h>

#include <chrono>
#include <optional>
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
#include "similarity/similarity_engine.h"
#include "utils/error.h"
#include "utils/structured_log.h"
#include "vectors/metadata_store.h"
#include "vectors/vector_store.h"

namespace nvecd::server {

namespace {

std::vector<std::pair<std::string, float>> ApplyMinScore(const std::vector<similarity::SimilarityResult>& results,
                                                         float min_score) {
  std::vector<std::pair<std::string, float>> pairs;
  pairs.reserve(results.size());
  for (const auto& item : results) {
    if (item.score >= min_score) {
      pairs.emplace_back(item.item_id, item.score);
    }
  }
  return pairs;
}

std::string AdaptiveCachePart(std::optional<bool> adaptive) {
  if (!adaptive.has_value()) {
    return "default";
  }
  return *adaptive ? "on" : "off";
}

std::vector<similarity::SimilarityResult> ApplyMetadataFilter(const std::vector<similarity::SimilarityResult>& results,
                                                              vectors::MetadataStore* metadata_store,
                                                              const vectors::MetadataFilter& filter) {
  if (filter.Empty() || metadata_store == nullptr) {
    return results;
  }

  std::vector<similarity::SimilarityResult> filtered;
  filtered.reserve(results.size());
  for (const auto& result : results) {
    if (metadata_store->Matches(result.item_id, filter)) {
      filtered.push_back(result);
    }
  }
  return filtered;
}

/// Oversampling factor used when a metadata filter is combined with an
/// events-mode search. The co-occurrence search has no filter awareness, so we
/// fetch more candidates than requested and filter afterwards; this keeps the
/// filtered result able to reach top_k when enough matching neighbors exist.
constexpr int kEventsFilterOversampling = 3;

/**
 * @brief Apply a metadata filter to events-mode results and truncate to top_k.
 *
 * Events mode resolves candidates purely from the co-occurrence index, which is
 * metadata-unaware. Filtering is therefore applied here against the metadata
 * store. Because MetadataStore::Matches keys off the item ID alone, a vectorless
 * item with matching metadata stays eligible, preserving the cold-start
 * recommendation use case. The caller over-fetches so that, after filtering,
 * up to @p top_k matching items can still be returned.
 *
 * @param results Over-fetched events-mode results (already score-sorted)
 * @param metadata_store Metadata store (may be null)
 * @param filter Filter conditions (empty = no filtering)
 * @param top_k Maximum number of results to keep
 * @return Filtered results truncated to top_k
 */
std::vector<similarity::SimilarityResult> ApplyEventsFilterTopK(
    const std::vector<similarity::SimilarityResult>& results, vectors::MetadataStore* metadata_store,
    const vectors::MetadataFilter& filter, int top_k) {
  std::vector<similarity::SimilarityResult> filtered;
  filtered.reserve(results.size());
  for (const auto& result : results) {
    if (filter.Empty() || metadata_store == nullptr || metadata_store->Matches(result.item_id, filter)) {
      filtered.push_back(result);
      if (top_k > 0 && static_cast<int>(filtered.size()) >= top_k) {
        break;
      }
    }
  }
  return filtered;
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

  // Atomically append the event and capture the prior buffer state so the
  // incremental co-occurrence delta is computed from a consistent view, even
  // under concurrent same-context ingestion.
  auto result =
      ctx_.event_store->AddEventAndGetPrior(cmd.ctx, cmd.id, cmd.score, cmd.event_type, cmd.timestamp.value_or(0));
  if (!result) {
    return utils::MakeUnexpected(result.error());
  }

  // Update co-occurrence index incrementally (only new pairs, once each).
  if (ctx_.co_index != nullptr && !result->deduped) {
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

  auto result = ctx_.vector_store->SetVector(cmd.id, cmd.vector);
  if (!result) {
    return utils::MakeUnexpected(result.error());
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

  // Selective cache invalidation for mutated item
  auto* cache_ptr = ctx_.cache.load(std::memory_order_acquire);
  if (cache_ptr != nullptr) {
    cache_ptr->InvalidateByItemId(cmd.id);
  }

  return FormatOK("VECSET");
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

  auto parsed = ParseSimpleFilter(cmd.filter_expr);
  if (!parsed) {
    return utils::MakeUnexpected(parsed.error());
  }

  vectors::Metadata metadata;
  for (const auto& condition : parsed->conditions) {
    if (condition.field.empty()) {
      return utils::MakeUnexpected(
          utils::MakeError(utils::ErrorCode::kCommandInvalidArgument, "Metadata key must not be empty"));
    }
    metadata[condition.field] = condition.value;
  }
  ctx_.metadata_store->Set(cmd.id, std::move(metadata));

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
    auto gen = ctx_.co_index != nullptr ? ctx_.co_index->GetGeneration() : 0;
    auto vgen = ctx_.vector_generation.load(std::memory_order_acquire);
    std::string key_str = "SIM:" + cmd.id + ":" + std::to_string(cmd.top_k) + ":" + cmd.mode + ":a" +
                          AdaptiveCachePart(cmd.adaptive) + ":g" + std::to_string(gen) + ":v" + std::to_string(vgen);
    if (!cmd.filter_expr.empty()) {
      key_str += ":f" + cmd.filter_expr;
    }
    cache_key = cache::CacheKeyGenerator::Generate(key_str);
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
    // Events mode resolves candidates from the co-occurrence index, which is
    // metadata-unaware. When a filter is active, over-fetch and filter against
    // the metadata store afterwards so the response can still reach top_k. A
    // vectorless item with matching metadata stays eligible (cold-start use
    // case), because matching keys off the item ID, not a stored vector row.
    const bool filtering = !filter.Empty() && ctx_.metadata_store != nullptr;
    // Clamp the over-fetch to the configured maximum so it never trips the
    // engine's own top_k upper-bound validation.
    const int max_top_k = ctx_.config != nullptr ? static_cast<int>(ctx_.config->similarity.max_top_k) : 0;
    int fetch_k = cmd.top_k;
    if (filtering) {
      fetch_k = cmd.top_k * kEventsFilterOversampling;
      if (max_top_k > 0 && fetch_k > max_top_k) {
        fetch_k = max_top_k;
      }
    }
    result = ctx_.similarity_engine->SearchByIdEvents(cmd.id, fetch_k);
    if (result && filtering) {
      *result = ApplyEventsFilterTopK(*result, ctx_.metadata_store, filter, cmd.top_k);
    }
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
    cache_ptr->Insert(cache_key, *result, elapsed_ms, search_type);

    // Register result items for selective cache invalidation
    std::vector<std::string> item_ids;
    item_ids.reserve(result->size() + 1);
    item_ids.push_back(cmd.id);  // Query ID itself
    for (const auto& item : *result) {
      item_ids.push_back(item.item_id);
    }
    cache_ptr->RegisterResultItems(cache_key, item_ids);
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
    std::string vec_hash = cache::HashVector(cmd.vector);
    auto vgen = ctx_.vector_generation.load(std::memory_order_acquire);
    std::string key_str = "SIMV:" + vec_hash + ":" + std::to_string(cmd.top_k) + ":v" + std::to_string(vgen);
    if (!cmd.filter_expr.empty()) {
      key_str += ":f" + cmd.filter_expr;
    }
    cache_key = cache::CacheKeyGenerator::Generate(key_str);
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
    cache_ptr->Insert(cache_key, *result, elapsed_ms, search_type);

    // Register result items for selective cache invalidation
    std::vector<std::string> item_ids;
    item_ids.reserve(result->size());
    for (const auto& item : *result) {
      item_ids.push_back(item.item_id);
    }
    cache_ptr->RegisterResultItems(cache_key, item_ids);
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

  if (cmd.variable_value == ctx_.requirepass) {
    conn_ctx.authenticated = true;
    return "+OK\r\n";
  }

  ctx_.stats.failed_commands++;
  return FormatError("ERR invalid password");
}

}  // namespace nvecd::server
