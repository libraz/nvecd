/**
 * @file similarity_result_utils.h
 * @brief Shared result post-processing for TCP and HTTP similarity handlers
 */

#pragma once

#include <string>
#include <utility>
#include <vector>

#include "similarity/similarity_result.h"

namespace nvecd::server {

inline std::vector<std::pair<std::string, float>> ApplyMinScore(
    const std::vector<similarity::SimilarityResult>& results, float min_score) {
  std::vector<std::pair<std::string, float>> filtered;
  filtered.reserve(results.size());
  for (const auto& item : results) {
    if (item.score >= min_score) {
      filtered.emplace_back(item.item_id, item.score);
    }
  }
  return filtered;
}

}  // namespace nvecd::server
