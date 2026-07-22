/**
 * @file cache_key_generator.h
 * @brief Cache key generation for similarity queries
 *
 * Reference: ../mygram-db/src/cache/query_normalizer.h
 * Reusability: 50% (concept reused, implementation is nvecd-specific)
 * Adapted for: SIM/SIMV query cache keys
 */

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "cache/cache_key.h"

namespace nvecd::cache {

/**
 * @brief Complete cache identity for an ID-based similarity query.
 *
 * Both the TCP and HTTP surfaces must use this exact set of fields. In
 * particular, vector_generation invalidates cached vector/fusion results when
 * either surface accepts a VECSET.
 */
struct SimCacheKeyParams {
  std::string id;
  int top_k = 0;
  std::string mode;
  std::optional<bool> adaptive;
  uint64_t cooccurrence_generation = 0;
  uint64_t vector_generation = 0;
  std::string filter_expr;
};

/**
 * @brief Complete cache identity for a vector-based similarity query.
 */
struct SimvCacheKeyParams {
  std::vector<float> vector;
  int top_k = 0;
  uint64_t vector_generation = 0;
  std::string filter_expr;
};

/**
 * @brief Generate the canonical cache key shared by TCP and HTTP SIM.
 */
CacheKey GenerateSimCacheKey(const SimCacheKeyParams& params);

/**
 * @brief Generate the canonical cache key shared by TCP and HTTP SIMV.
 */
CacheKey GenerateSimvCacheKey(const SimvCacheKeyParams& params);

/**
 * @brief Hash vector to string (for cache key generation)
 *
 * @param vector Input vector
 * @return Hex string representation of MD5 hash
 *
 * Implementation: MD5(bytes of float array)
 */
std::string HashVector(const std::vector<float>& vector);

}  // namespace nvecd::cache
