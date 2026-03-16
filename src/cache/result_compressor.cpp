/**
 * @file result_compressor.cpp
 * @brief LZ4 compression implementation
 *
 * Reference: ../mygram-db/src/cache/result_compressor.cpp
 * Reusability: 90% (added SimilarityResult support)
 * Adapted for: nvecd similarity search caching
 */

#include "cache/result_compressor.h"

#include <lz4.h>

#include <cstring>

namespace nvecd::cache {

// Helper struct for serialization (POD type for LZ4 compression)
// id: 256-byte fixed buffer, score: 4-byte float = 260 bytes per result
struct SerializedSimilarityResult {
  char id[256];  // Fixed-size ID buffer
  float score;
};

utils::Expected<std::vector<uint8_t>, utils::Error> ResultCompressor::CompressSimilarityResults(
    const std::vector<similarity::SimilarityResult>& results) {
  if (results.empty()) {
    return std::vector<uint8_t>{};
  }

  // Serialize SimilarityResult to POD format
  std::vector<SerializedSimilarityResult> serialized;
  serialized.reserve(results.size());

  for (const auto& result : results) {
    SerializedSimilarityResult ser{};
    std::strncpy(ser.id, result.item_id.c_str(), sizeof(ser.id) - 1);
    ser.id[sizeof(ser.id) - 1] = '\0';  // Ensure null-termination
    ser.score = result.score;
    serialized.push_back(ser);
  }

  const size_t src_size = serialized.size() * sizeof(SerializedSimilarityResult);
  // reinterpret_cast required for LZ4 C API (expects char*)
  const auto* src_data =
      reinterpret_cast<const char*>(serialized.data());  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)

  // Calculate maximum compressed size
  const int max_dst_size = LZ4_compressBound(static_cast<int>(src_size));
  if (max_dst_size <= 0) {
    return utils::MakeUnexpected(
        utils::MakeError(utils::ErrorCode::kCacheCompressionFailed, "LZ4_compressBound failed"));
  }

  std::vector<uint8_t> compressed(static_cast<size_t>(max_dst_size));

  // Compress with default compression level (fast)
  // reinterpret_cast required for LZ4 C API (expects char*)
  const int compressed_size = LZ4_compress_default(
      src_data, reinterpret_cast<char*>(compressed.data()),  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
      static_cast<int>(src_size), max_dst_size);

  if (compressed_size <= 0) {
    return utils::MakeUnexpected(utils::MakeError(utils::ErrorCode::kCacheCompressionFailed, "LZ4 compression failed"));
  }

  // Resize to actual compressed size
  compressed.resize(static_cast<size_t>(compressed_size));
  return compressed;
}

utils::Expected<std::vector<similarity::SimilarityResult>, utils::Error> ResultCompressor::DecompressSimilarityResults(
    const std::vector<uint8_t>& compressed, size_t original_size) {
  if (compressed.empty() || original_size == 0) {
    return std::vector<similarity::SimilarityResult>{};
  }

  // original_size is in bytes
  const size_t num_results = original_size / sizeof(SerializedSimilarityResult);

  // Allocate buffer for serialized data
  std::vector<SerializedSimilarityResult> serialized(num_results);

  // Decompress
  // reinterpret_cast required for LZ4 C API (expects char*)
  const int decompressed_size = LZ4_decompress_safe(
      reinterpret_cast<const char*>(compressed.data()),  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
      reinterpret_cast<char*>(serialized.data()),        // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
      static_cast<int>(compressed.size()), static_cast<int>(original_size));

  if (decompressed_size < 0) {
    return utils::MakeUnexpected(
        utils::MakeError(utils::ErrorCode::kCacheDecompressionFailed, "LZ4 decompression failed"));
  }

  if (static_cast<size_t>(decompressed_size) != original_size) {
    return utils::MakeUnexpected(utils::MakeError(utils::ErrorCode::kCacheDecompressionFailed,
                                                  "LZ4 decompression size mismatch: expected " +
                                                      std::to_string(original_size) + " bytes, got " +
                                                      std::to_string(decompressed_size) + " bytes"));
  }

  // Deserialize to SimilarityResult
  std::vector<similarity::SimilarityResult> results;
  results.reserve(num_results);

  for (const auto& ser : serialized) {
    results.emplace_back(std::string(ser.id), ser.score);
  }

  return results;
}

}  // namespace nvecd::cache
