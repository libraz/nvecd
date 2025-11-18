/**
 * @file cache_key_generator.h
 * @brief Cache key generation for similarity queries
 *
 * Reference: ../mygram-db/src/cache/query_normalizer.h
 * Reusability: 50% (concept reused, implementation is nvecd-specific)
 * Adapted for: SIM/SIMV query cache keys
 */

#pragma once

#include <string>
#include <vector>

#include "cache/cache_key.h"

namespace nvecd::cache {

/**
 * @brief Generate cache key for SIM command (ID-based similarity search)
 *
 * @param id Target ID
 * @param top_k Number of results
 * @param mode Search mode ("vectors", "events", "fusion")
 * @return MD5-based cache key
 *
 * Key format: "SIM:<id>:<top_k>:<mode>"
 */
CacheKey GenerateSimCacheKey(const std::string& id, int top_k, const std::string& mode);

/**
 * @brief Generate cache key for SIMV command (vector-based similarity search)
 *
 * @param vector Query vector
 * @param top_k Number of results
 * @param mode Search mode ("vectors", "events", "fusion")
 * @return MD5-based cache key
 *
 * Key format: "SIMV:<vector_hash>:<top_k>:<mode>"
 *
 * Note: Vector is hashed using MD5 of raw bytes for deterministic key generation.
 * This ensures identical vectors produce identical cache keys.
 */
CacheKey GenerateSimvCacheKey(const std::vector<float>& vector, int top_k, const std::string& mode);

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
