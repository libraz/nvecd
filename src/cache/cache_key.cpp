/**
 * @file cache_key.cpp
 * @brief Cache key generation implementation
 *
 * Reference: ../mygram-db/src/cache/cache_key.cpp
 * Reusability: 100% (namespace change only)
 * Adapted for: nvecd similarity search caching
 */

#include "cache/cache_key.h"

#include <iomanip>
#include <sstream>

#include "cache/md5.h"

namespace nvecd::cache {

std::string CacheKey::ToString() const {
  constexpr int kHexWidth = 16;
  std::ostringstream oss;
  oss << std::hex << std::setfill('0');
  oss << std::setw(kHexWidth) << hash_high;
  oss << std::setw(kHexWidth) << hash_low;
  return oss.str();
}

CacheKey CacheKeyGenerator::Generate(const std::string& normalized_query) {
  constexpr size_t kHalfDigestSize = MD5::kDigestSize / 2;
  constexpr int kBitsPerByte = 8;

  uint8_t digest[MD5::kDigestSize];     // NOLINT(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)
  MD5::Hash(normalized_query, digest);  // NOLINT(cppcoreguidelines-pro-bounds-array-to-pointer-decay)

  // Extract two 64-bit values from MD5 digest (128 bits total)
  uint64_t hash_high = 0;
  uint64_t hash_low = 0;

  // High 64 bits (bytes 0-7)
  for (size_t i = 0; i < kHalfDigestSize; ++i) {
    hash_high = (hash_high << kBitsPerByte) | digest[i];  // NOLINT(cppcoreguidelines-pro-bounds-constant-array-index)
  }

  // Low 64 bits (bytes 8-15)
  for (size_t i = kHalfDigestSize; i < MD5::kDigestSize; ++i) {
    hash_low = (hash_low << kBitsPerByte) | digest[i];  // NOLINT(cppcoreguidelines-pro-bounds-constant-array-index)
  }

  return {hash_high, hash_low};
}

}  // namespace nvecd::cache
