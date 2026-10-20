/**
 * @file container_memory.h
 * @brief Bookkeeping estimates for node-based standard containers
 */

#pragma once

#include <cstddef>

namespace nvecd::utils {

/**
 * @brief Estimated bookkeeping bytes of an unordered container
 *
 * Counts the bucket array plus, per node, the forward link and the cached hash
 * that libstdc++/libc++ keep beside the value. The values themselves (and any
 * heap behind them) are the caller's to count.
 */
template <typename Map>
size_t UnorderedBookkeeping(const Map& map) {
  return map.bucket_count() * sizeof(void*) + map.size() * (sizeof(void*) + sizeof(size_t));
}

/// @brief Estimated bookkeeping bytes per std::list node (prev and next links).
constexpr size_t kListNodeOverhead = 2 * sizeof(void*);

}  // namespace nvecd::utils
