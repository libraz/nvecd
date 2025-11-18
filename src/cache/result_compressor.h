/**
 * @file result_compressor.h
 * @brief LZ4 compression for cached search results
 *
 * Reference: ../mygram-db/src/cache/result_compressor.h
 * Reusability: 90% (added SimilarityResult support)
 * Adapted for: nvecd similarity search caching
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "similarity/similarity_engine.h"

namespace nvecd::cache {

/**
 * @brief Compress and decompress search results using LZ4
 *
 * LZ4 provides fast compression (500+ MB/s) and very fast decompression (2+ GB/s),
 * making it ideal for query cache where latency is critical.
 * Typical compression ratio: 2-3x for search results.
 *
 * Supports compression of SimilarityResult vectors (id + score).
 */
class ResultCompressor {
 public:
  /**
   * @brief Compress vector of similarity results
   * @param results Vector of similarity results to compress
   * @return Compressed data
   * @throws std::runtime_error if compression fails
   */
  static std::vector<uint8_t> CompressSimilarityResults(const std::vector<similarity::SimilarityResult>& results);

  /**
   * @brief Decompress to vector of similarity results
   * @param compressed Compressed data
   * @param original_size Original uncompressed size in bytes
   * @return Decompressed vector of similarity results
   * @throws std::runtime_error if decompression fails or size mismatch
   */
  static std::vector<similarity::SimilarityResult> DecompressSimilarityResults(const std::vector<uint8_t>& compressed,
                                                                               size_t original_size);
};

}  // namespace nvecd::cache
