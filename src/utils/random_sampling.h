/**
 * @file random_sampling.h
 * @brief Uniform sampling of unique indices without scanning the full range.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <numeric>
#include <random>
#include <unordered_set>
#include <vector>

namespace nvecd::utils {

/**
 * @brief Return a uniform subset of @p sample_size distinct indices from [0, total).
 *
 * Floyd's algorithm makes the partial-sample path expected O(sample_size),
 * independent of the size of the population. The output order is unspecified.
 */
template <typename URBG>
std::vector<std::size_t> SampleIndices(std::size_t total, std::size_t sample_size, URBG& rng) {
  if (sample_size >= total) {
    std::vector<std::size_t> all(total);
    std::iota(all.begin(), all.end(), std::size_t{0});
    return all;
  }

  if (sample_size == 0) {
    return {};
  }

  // Dense flags beat hashing for near-dense samples; total / k <= 8 keeps them O(k).
  const bool use_dense_flags = total / sample_size <= 8;
  std::vector<std::uint8_t> selected_flags;
  std::unordered_set<std::size_t> selected_set;
  if (use_dense_flags) {
    selected_flags.resize(total, 0);
  } else {
    selected_set.reserve(sample_size);
  }

  const auto insert_selected = [&](std::size_t index) {
    if (use_dense_flags) {
      if (selected_flags[index] != 0) {
        return false;
      }
      selected_flags[index] = 1;
      return true;
    }
    return selected_set.insert(index).second;
  };

  std::vector<std::size_t> indices;
  indices.reserve(sample_size);

  const std::size_t first = total - sample_size;
  for (std::size_t offset = 0; offset < sample_size; ++offset) {
    const std::size_t j = first + offset;
    std::uniform_int_distribution<std::size_t> distribution(0, j);
    const std::size_t candidate = distribution(rng);
    if (insert_selected(candidate)) {
      indices.push_back(candidate);
    } else {
      // j was out of range in every earlier draw, so taking it keeps the subset uniform.
      insert_selected(j);
      indices.push_back(j);
    }
  }

  return indices;
}

}  // namespace nvecd::utils
