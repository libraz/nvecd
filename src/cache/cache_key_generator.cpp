/**
 * @file cache_key_generator.cpp
 * @brief Cache key generation implementation
 */

#include "cache/cache_key_generator.h"

#include <cstdint>
#include <cstring>
#include <iomanip>
#include <sstream>

#include "cache/md5.h"

namespace nvecd::cache {

namespace {

std::string AdaptiveCachePart(std::optional<bool> adaptive) {
  if (!adaptive.has_value()) {
    return "default";
  }
  return *adaptive ? "on" : "off";
}

}  // namespace

CacheKey GenerateSimCacheKey(const SimCacheKeyParams& params) {
  std::ostringstream oss;
  oss << "SIM:" << params.id << ":" << params.top_k << ":" << params.mode << ":a" << AdaptiveCachePart(params.adaptive)
      << ":g" << params.cooccurrence_generation << ":v" << params.vector_generation;
  if (!params.filter_expr.empty()) {
    oss << ":f" << params.filter_expr;
  }
  return CacheKeyGenerator::Generate(oss.str());
}

CacheKey GenerateSimvCacheKey(const SimvCacheKeyParams& params) {
  std::ostringstream oss;
  oss << "SIMV:" << HashVector(params.vector) << ":" << params.top_k << ":v" << params.vector_generation;
  if (!params.filter_expr.empty()) {
    oss << ":f" << params.filter_expr;
  }
  return CacheKeyGenerator::Generate(oss.str());
}

std::string HashVector(const std::vector<float>& vector) {
  if (vector.empty()) {
    return std::string(32, '0');  // Return zero hash for empty vector
  }

  // Guard against integer overflow
  if (vector.size() > SIZE_MAX / sizeof(float)) {
    return std::string(32, '0');  // Return zero hash on overflow
  }

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
