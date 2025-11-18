/**
 * @file cache_key_generator.cpp
 * @brief Cache key generation implementation
 */

#include "cache/cache_key_generator.h"

#include <cstring>
#include <iomanip>
#include <sstream>

#include "cache/md5.h"

namespace nvecd::cache {

CacheKey GenerateSimCacheKey(const std::string& id, int top_k, const std::string& mode) {
  // Format: "SIM:<id>:<top_k>:<mode>"
  std::ostringstream oss;
  oss << "SIM:" << id << ":" << top_k << ":" << mode;
  return CacheKeyGenerator::Generate(oss.str());
}

CacheKey GenerateSimvCacheKey(const std::vector<float>& vector, int top_k, const std::string& mode) {
  // Format: "SIMV:<vector_hash>:<top_k>:<mode>"
  std::string vector_hash = HashVector(vector);
  std::ostringstream oss;
  oss << "SIMV:" << vector_hash << ":" << top_k << ":" << mode;
  return CacheKeyGenerator::Generate(oss.str());
}

std::string HashVector(const std::vector<float>& vector) {
  // Hash raw bytes of vector using MD5
  const uint8_t* data =
      reinterpret_cast<const uint8_t*>(vector.data());  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
  size_t size_bytes = vector.size() * sizeof(float);

  uint8_t digest[16];
  MD5 md5;
  md5.Update(data, size_bytes);
  md5.Finalize(digest);

  // Convert digest to hex string
  std::ostringstream oss;
  oss << std::hex << std::setfill('0');
  for (int i = 0; i < 16; ++i) {
    oss << std::setw(2) << static_cast<unsigned int>(digest[i]);
  }
  return oss.str();
}

}  // namespace nvecd::cache
