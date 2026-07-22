/**
 * @file similarity_result_utils.h
 * @brief Shared result post-processing for TCP and HTTP similarity handlers
 */

#pragma once

#include <string>
#include <utility>
#include <vector>

#include "similarity/similarity_engine.h"
#include "vectors/metadata_store.h"

namespace nvecd::server {

inline constexpr int kEventsFilterOversampling = 3;

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

inline std::vector<similarity::SimilarityResult> ApplyMetadataFilter(
    const std::vector<similarity::SimilarityResult>& results, vectors::MetadataStore* metadata_store,
    const vectors::MetadataFilter& filter) {
  if (filter.Empty() || metadata_store == nullptr) {
    return results;
  }
  std::vector<similarity::SimilarityResult> filtered;
  filtered.reserve(results.size());
  for (const auto& item : results) {
    if (metadata_store->Matches(item.item_id, filter)) {
      filtered.push_back(item);
    }
  }
  return filtered;
}

inline std::vector<similarity::SimilarityResult> ApplyEventsFilterTopK(
    const std::vector<similarity::SimilarityResult>& results, vectors::MetadataStore* metadata_store,
    const vectors::MetadataFilter& filter, int top_k) {
  std::vector<similarity::SimilarityResult> filtered;
  filtered.reserve(results.size());
  for (const auto& item : results) {
    if (filter.Empty() || metadata_store == nullptr || metadata_store->Matches(item.item_id, filter)) {
      filtered.push_back(item);
      if (top_k > 0 && static_cast<int>(filtered.size()) >= top_k) {
        break;
      }
    }
  }
  return filtered;
}

}  // namespace nvecd::server
