/**
 * @file info_handler.h
 * @brief INFO command handler
 *
 * Reference: ../mygram-db/src/server/handlers/info_handler.h
 * Reusability: 90% (same INFO format, different stats)
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "server/server_types.h"
#include "utils/error.h"
#include "utils/expected.h"

namespace nvecd::server::handlers {

/**
 * @brief Memory figures reported by INFO, HTTP /info and /metrics
 *
 * Computed in one place so the surfaces cannot disagree on what a name means.
 */
struct MemoryFigures {
  /// Vector matrix only: vector_count x dimension x sizeof(float). Reported as used_memory_bytes.
  uint64_t vector_matrix_bytes = 0;
  /// Allocated bytes of each store, including indexes and bookkeeping.
  size_t events_bytes = 0;
  size_t vectors_bytes = 0;
  size_t co_index_bytes = 0;

  size_t StoresTotal() const { return events_bytes + vectors_bytes + co_index_bytes; }
};

/// Read the memory figures from the stores in @p ctx; absent stores count as zero.
MemoryFigures GatherMemoryFigures(const HandlerContext& ctx);

/// MemoryFigures::vector_matrix_bytes alone, without walking any store.
uint64_t VectorMatrixBytes(const HandlerContext& ctx);

/**
 * @brief Cache whose figures INFO, /info and /metrics report
 *
 * The controller's cache whether or not it is enabled, since disabling keeps
 * its entries; otherwise the published cache. Null when there is neither.
 */
const cache::SimilarityCache* ReportedCache(const HandlerContext& ctx);

/**
 * @brief Handle INFO command
 *
 * Returns Redis-style server statistics:
 * - Server info (version, uptime)
 * - Stats (commands processed, connections) and per-group command stats
 * - Memory usage
 * - Data counts (events, vectors, contexts)
 *
 * @param ctx Handler context
 * @return INFO response string or error
 */
utils::Expected<std::string, utils::Error> HandleInfo(const HandlerContext& ctx);

}  // namespace nvecd::server::handlers
