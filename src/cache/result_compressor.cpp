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

#include <algorithm>
#include <climits>
#include <cstring>
#include <iterator>

namespace nvecd::cache {

// Helper struct for serialization (POD type for LZ4 compression)
// id: 256-byte fixed buffer, score: 4-byte float = 260 bytes per result
struct SerializedSimilarityResult {
  char id[256];  // Fixed-size ID buffer
  float score;
};

utils::Expected<std::vector<uint8_t>, utils::Error> ResultCompressor::SerializeSimilarityResults(
    const std::vector<similarity::SimilarityResult>& results) {
  if (results.empty()) {
    return std::vector<uint8_t>{};
  }
  if (results.size() > static_cast<size_t>(INT_MAX) / sizeof(SerializedSimilarityResult)) {
    return utils::MakeUnexpected(
        utils::MakeError(utils::ErrorCode::kCacheCompressionFailed, "Serialized cache result size exceeds limit"));
  }

  std::vector<uint8_t> serialized(results.size() * sizeof(SerializedSimilarityResult));

  for (size_t index = 0; index < results.size(); ++index) {
    const auto& result = results[index];
    // The fixed buffer must hold the id plus a null terminator. An id that does
    // not fit cannot be stored without silent truncation, which would make a
    // cache hit return a different (truncated) id than the uncached path.
    // Reject such results so the entry is never cached truncated and lookups
    // fall through to the authoritative uncached path.
    if (result.item_id.size() >= sizeof(SerializedSimilarityResult::id) ||
        result.item_id.find('\0') != std::string::npos) {
      return utils::MakeUnexpected(utils::MakeError(utils::ErrorCode::kCacheCompressionFailed,
                                                    "item_id is too long or contains an embedded NUL byte"));
    }

    SerializedSimilarityResult ser{};
    std::memcpy(ser.id, result.item_id.c_str(), result.item_id.size());
    ser.id[result.item_id.size()] = '\0';  // Ensure null-termination
    ser.score = result.score;
    std::memcpy(serialized.data() + index * sizeof(SerializedSimilarityResult), &ser,
                sizeof(SerializedSimilarityResult));
  }
  return serialized;
}

utils::Expected<std::vector<similarity::SimilarityResult>, utils::Error> ResultCompressor::DeserializeSimilarityResults(
    const std::vector<uint8_t>& serialized) {
  if (serialized.empty()) {
    return std::vector<similarity::SimilarityResult>{};
  }
  if (serialized.size() % sizeof(SerializedSimilarityResult) != 0 || serialized.size() > static_cast<size_t>(INT_MAX)) {
    return utils::MakeUnexpected(
        utils::MakeError(utils::ErrorCode::kCacheDecompressionFailed, "Invalid serialized cache result size"));
  }
  const size_t count = serialized.size() / sizeof(SerializedSimilarityResult);
  std::vector<similarity::SimilarityResult> results;
  results.reserve(count);
  for (size_t index = 0; index < count; ++index) {
    SerializedSimilarityResult value{};
    std::memcpy(&value, serialized.data() + index * sizeof(SerializedSimilarityResult), sizeof(value));
    const auto terminator = std::find(std::begin(value.id), std::end(value.id), '\0');
    if (terminator == std::end(value.id)) {
      return utils::MakeUnexpected(
          utils::MakeError(utils::ErrorCode::kCacheDecompressionFailed, "Cached result ID is not terminated"));
    }
    results.emplace_back(std::string(value.id, terminator), value.score);
  }
  return results;
}

utils::Expected<std::vector<uint8_t>, utils::Error> ResultCompressor::CompressSimilarityResults(
    const std::vector<similarity::SimilarityResult>& results) {
  auto serialized = SerializeSimilarityResults(results);
  if (!serialized) {
    return utils::MakeUnexpected(serialized.error());
  }
  if (serialized->empty()) {
    return std::vector<uint8_t>{};
  }

  const size_t src_size = serialized->size();
  // reinterpret_cast required for LZ4 C API (expects char*)
  const auto* src_data =
      reinterpret_cast<const char*>(serialized->data());  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)

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
  if (original_size % sizeof(SerializedSimilarityResult) != 0 || original_size > static_cast<size_t>(INT_MAX) ||
      compressed.size() > static_cast<size_t>(INT_MAX)) {
    return utils::MakeUnexpected(
        utils::MakeError(utils::ErrorCode::kCacheDecompressionFailed, "Invalid compressed cache result size"));
  }

  std::vector<uint8_t> serialized(original_size);

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

  return DeserializeSimilarityResults(serialized);
}

}  // namespace nvecd::cache
