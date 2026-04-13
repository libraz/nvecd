/**
 * @file tiered_vector_store.cpp
 * @brief Main/Delta two-tier vector storage implementation
 */

#include "vectors/tiered_vector_store.h"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <queue>
#include <utility>

#include "vectors/distance.h"
#include "vectors/hnsw_index.h"

namespace nvecd::vectors {

TieredVectorStore::TieredVectorStore(const Config& config)
    : config_(config), distance_func_(GetDistanceFunc(config.distance_metric)) {}

// ============================================================================
// Core API
// ============================================================================

utils::Expected<void, utils::Error> TieredVectorStore::Add(const std::string& id, std::vector<float> vec) {
  std::unique_lock lock(mutex_);

  // Set dimension on first insert
  if (dimension_ == 0) {
    if (vec.empty()) {
      return utils::MakeUnexpected(
          utils::MakeError(utils::ErrorCode::kVectorInvalidDimension, "Cannot add empty vector"));
    }
    dimension_ = static_cast<uint32_t>(vec.size());
  }

  // Validate dimension
  if (vec.size() != dimension_) {
    return utils::MakeUnexpected(
        utils::MakeError(utils::ErrorCode::kVectorDimensionMismatch,
                         "Expected dimension " + std::to_string(dimension_) + ", got " + std::to_string(vec.size())));
  }

  // If ID already exists, delete first
  auto loc_it = id_location_.find(id);
  if (loc_it != id_location_.end()) {
    if (loc_it->second.store == StoreLocation::kMain) {
      main_.deleted.insert(loc_it->second.compact_index);
    } else {
      RemoveFromDelta(loc_it->second.compact_index);
    }
    id_location_.erase(loc_it);
  }

  // Append to delta
  auto delta_idx = static_cast<uint32_t>(delta_.ids.size());
  delta_.matrix.insert(delta_.matrix.end(), vec.begin(), vec.end());
  delta_.norms.push_back(ComputeNorm(vec.data(), dimension_));
  delta_.ids.push_back(id);

  id_location_[id] = {StoreLocation::kDelta, delta_idx};

  return {};
}

utils::Expected<void, utils::Error> TieredVectorStore::Delete(const std::string& id) {
  std::unique_lock lock(mutex_);

  auto loc_it = id_location_.find(id);
  if (loc_it == id_location_.end()) {
    return utils::MakeUnexpected(utils::MakeError(utils::ErrorCode::kNotFound, "Vector not found: " + id));
  }

  if (loc_it->second.store == StoreLocation::kMain) {
    main_.deleted.insert(loc_it->second.compact_index);
    if (main_.index) {
      main_.index->MarkDeleted(loc_it->second.compact_index);
    }
  } else {
    RemoveFromDelta(loc_it->second.compact_index);
  }

  id_location_.erase(loc_it);
  return {};
}

utils::Expected<void, utils::Error> TieredVectorStore::Update(const std::string& id, std::vector<float> vec) {
  // Add handles delete-if-exists + insert
  return Add(id, std::move(vec));
}

std::vector<TieredSearchResult> TieredVectorStore::Search(const float* query, uint32_t top_k) const {
  std::shared_lock lock(mutex_);

  if (dimension_ == 0 || top_k == 0) {
    return {};
  }

  auto main_results = SearchMain(query, top_k);
  auto delta_results = SearchDelta(query, top_k);

  return MergeResults(main_results, delta_results, top_k);
}

// ============================================================================
// Merge / Rebuild
// ============================================================================

utils::Expected<void, utils::Error> TieredVectorStore::MergeDeltaToMain() {
  std::unique_lock lock(mutex_);

  if (delta_.Size() == 0) {
    return {};
  }

  // Append delta vectors to main
  for (size_t i = 0; i < delta_.ids.size(); ++i) {
    auto main_idx = static_cast<uint32_t>(main_.ids.size());
    const float* vec_ptr = &delta_.matrix[i * dimension_];
    main_.matrix.insert(main_.matrix.end(), vec_ptr, vec_ptr + dimension_);
    main_.norms.push_back(delta_.norms[i]);
    main_.ids.push_back(delta_.ids[i]);

    // Update location: delta -> main
    id_location_[delta_.ids[i]] = {StoreLocation::kMain, main_idx};
  }

  delta_.Clear();

  // Rebuild main index with all current data
  RebuildMainIndex();

  return {};
}

utils::Expected<void, utils::Error> TieredVectorStore::RebuildMain() {
  std::unique_lock lock(mutex_);

  if (main_.deleted.empty()) {
    return {};
  }

  // Compact: remove deleted entries
  std::vector<float> new_matrix;
  std::vector<float> new_norms;
  std::vector<std::string> new_ids;
  uint32_t new_count = 0;

  for (uint32_t i = 0; i < main_.TotalSlots(); ++i) {
    if (main_.deleted.count(i) > 0) {
      continue;
    }
    const float* row = &main_.matrix[static_cast<size_t>(i) * dimension_];
    new_matrix.insert(new_matrix.end(), row, row + dimension_);
    new_norms.push_back(main_.norms[i]);
    new_ids.push_back(std::move(main_.ids[i]));

    // Update id_location_ for compacted entry
    id_location_[new_ids.back()] = {StoreLocation::kMain, new_count};
    ++new_count;
  }

  main_.matrix = std::move(new_matrix);
  main_.norms = std::move(new_norms);
  main_.ids = std::move(new_ids);
  main_.deleted.clear();

  // Rebuild index
  RebuildMainIndex();

  return {};
}

// ============================================================================
// Accessors
// ============================================================================

size_t TieredVectorStore::MainSize() const {
  std::shared_lock lock(mutex_);
  return main_.ActiveSize();
}

size_t TieredVectorStore::DeltaSize() const {
  std::shared_lock lock(mutex_);
  return delta_.Size();
}

size_t TieredVectorStore::TotalSize() const {
  std::shared_lock lock(mutex_);
  return main_.ActiveSize() + delta_.Size();
}

size_t TieredVectorStore::DeletedCount() const {
  std::shared_lock lock(mutex_);
  return main_.deleted.size();
}

uint32_t TieredVectorStore::GetDimension() const {
  std::shared_lock lock(mutex_);
  return dimension_;
}

bool TieredVectorStore::NeedsMerge() const {
  std::shared_lock lock(mutex_);
  return delta_.Size() >= config_.delta_merge_threshold;
}

bool TieredVectorStore::NeedsRebuild() const {
  std::shared_lock lock(mutex_);
  if (main_.TotalSlots() == 0) {
    return false;
  }
  auto ratio = static_cast<float>(main_.deleted.size()) / static_cast<float>(main_.TotalSlots());
  return ratio > config_.tombstone_ratio_threshold;
}

bool TieredVectorStore::HasVector(const std::string& id) const {
  std::shared_lock lock(mutex_);
  return id_location_.count(id) > 0;
}

bool TieredVectorStore::IsInMain(const std::string& id) const {
  std::shared_lock lock(mutex_);
  auto it = id_location_.find(id);
  return it != id_location_.end() && it->second.store == StoreLocation::kMain;
}

bool TieredVectorStore::IsInDelta(const std::string& id) const {
  std::shared_lock lock(mutex_);
  auto it = id_location_.find(id);
  return it != id_location_.end() && it->second.store == StoreLocation::kDelta;
}

// ============================================================================
// Private helpers
// ============================================================================

std::vector<TieredSearchResult> TieredVectorStore::SearchMain(const float* query, uint32_t top_k) const {
  if (main_.ActiveSize() == 0) {
    return {};
  }

  std::vector<TieredSearchResult> results;

  if (main_.index) {
    // ANN search: request extra candidates to compensate for deleted entries
    uint32_t fetch_k = top_k + static_cast<uint32_t>(main_.deleted.size());
    auto ann_results = main_.index->Search(query, fetch_k);

    for (const auto& [idx, score] : ann_results) {
      if (main_.deleted.count(idx) > 0) {
        continue;
      }
      results.push_back({main_.ids[idx], score});
      if (results.size() >= top_k) {
        break;
      }
    }
  } else {
    // Brute-force search on main matrix
    // Min-heap: (score, index) — keeps worst score on top for eviction
    using ScorePair = std::pair<float, uint32_t>;
    std::priority_queue<ScorePair, std::vector<ScorePair>, std::greater<ScorePair>> heap;

    for (uint32_t i = 0; i < main_.TotalSlots(); ++i) {
      if (main_.deleted.count(i) > 0) {
        continue;
      }
      const float* vec = &main_.matrix[static_cast<size_t>(i) * dimension_];
      float score = distance_func_(query, vec, dimension_);

      if (heap.size() < top_k) {
        heap.push({score, i});
      } else if (score > heap.top().first) {
        heap.pop();
        heap.push({score, i});
      }
    }

    results.reserve(heap.size());
    while (!heap.empty()) {
      auto [score, idx] = heap.top();
      heap.pop();
      results.push_back({main_.ids[idx], score});
    }
    // Reverse to get descending score order
    std::reverse(results.begin(), results.end());
  }

  return results;
}

std::vector<TieredSearchResult> TieredVectorStore::SearchDelta(const float* query, uint32_t top_k) const {
  if (delta_.Size() == 0) {
    return {};
  }

  using ScorePair = std::pair<float, uint32_t>;
  std::priority_queue<ScorePair, std::vector<ScorePair>, std::greater<ScorePair>> heap;

  for (uint32_t i = 0; i < static_cast<uint32_t>(delta_.ids.size()); ++i) {
    const float* vec = &delta_.matrix[static_cast<size_t>(i) * dimension_];
    float score = distance_func_(query, vec, dimension_);

    if (heap.size() < top_k) {
      heap.push({score, i});
    } else if (score > heap.top().first) {
      heap.pop();
      heap.push({score, i});
    }
  }

  std::vector<TieredSearchResult> results;
  results.reserve(heap.size());
  while (!heap.empty()) {
    auto [score, idx] = heap.top();
    heap.pop();
    results.push_back({delta_.ids[idx], score});
  }
  std::reverse(results.begin(), results.end());
  return results;
}

std::vector<TieredSearchResult> TieredVectorStore::MergeResults(std::vector<TieredSearchResult>& main_results,
                                                                std::vector<TieredSearchResult>& delta_results,
                                                                uint32_t top_k) {
  // Both inputs are sorted by score descending; merge them
  std::vector<TieredSearchResult> merged;
  merged.reserve(std::min(static_cast<size_t>(top_k), main_results.size() + delta_results.size()));

  size_t mi = 0;
  size_t di = 0;

  while (merged.size() < top_k && (mi < main_results.size() || di < delta_results.size())) {
    float main_score = (mi < main_results.size()) ? main_results[mi].score : -1e30F;
    float delta_score = (di < delta_results.size()) ? delta_results[di].score : -1e30F;

    if (main_score >= delta_score) {
      merged.push_back(std::move(main_results[mi]));
      ++mi;
    } else {
      merged.push_back(std::move(delta_results[di]));
      ++di;
    }
  }

  return merged;
}

void TieredVectorStore::RemoveFromDelta(uint32_t delta_idx) {
  uint32_t last = static_cast<uint32_t>(delta_.ids.size()) - 1;

  if (delta_idx != last) {
    // Swap with last element
    std::swap(delta_.ids[delta_idx], delta_.ids[last]);
    delta_.norms[delta_idx] = delta_.norms[last];

    // Copy last row's vector data to the deleted row
    std::memcpy(&delta_.matrix[static_cast<size_t>(delta_idx) * dimension_],
                &delta_.matrix[static_cast<size_t>(last) * dimension_], dimension_ * sizeof(float));

    // Update location for the swapped element
    id_location_[delta_.ids[delta_idx]] = {StoreLocation::kDelta, delta_idx};
  }

  // Remove last element
  delta_.ids.pop_back();
  delta_.norms.pop_back();
  delta_.matrix.resize(delta_.ids.size() * dimension_);
}

void TieredVectorStore::RebuildMainIndex() {
  if (main_.ActiveSize() == 0) {
    main_.index.reset();
    return;
  }

  HnswIndex::Config hnsw_cfg;
  hnsw_cfg.m = config_.hnsw_m;
  hnsw_cfg.ef_construction = config_.hnsw_ef_construction;
  hnsw_cfg.ef_search = config_.hnsw_ef_search;

  auto new_index = std::make_unique<HnswIndex>(dimension_, distance_func_, hnsw_cfg);

  // Add all non-deleted main vectors
  for (uint32_t i = 0; i < main_.TotalSlots(); ++i) {
    if (main_.deleted.count(i) > 0) {
      continue;
    }
    const float* vec = &main_.matrix[static_cast<size_t>(i) * dimension_];
    new_index->Add(i, vec);
  }

  main_.index = std::move(new_index);
}

float TieredVectorStore::ComputeNorm(const float* vec, uint32_t dim) {
  return simd::GetOptimalImpl().l2_norm(vec, dim);
}

}  // namespace nvecd::vectors
