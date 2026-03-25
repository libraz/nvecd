/**
 * @file similarity_engine.cpp
 * @brief Similarity engine implementation
 */

#include "similarity/similarity_engine.h"

#include <algorithm>
#include <map>
#include <numeric>
#include <random>
#include <unordered_set>

#include "utils/error.h"
#include "utils/structured_log.h"
#include "vectors/distance.h"

namespace nvecd::similarity {

SimilarityEngine::SimilarityEngine(events::EventStore* event_store, events::CoOccurrenceIndex* co_index,
                                   vectors::VectorStore* vector_store, const config::SimilarityConfig& config,
                                   const config::VectorsConfig& vectors_config)
    : event_store_(event_store),
      co_index_(co_index),
      vector_store_(vector_store),
      config_(config),
      distance_func_(SelectDistanceFunction(vectors_config.distance_metric)),
      use_prenorm_(vectors_config.distance_metric.empty() || vectors_config.distance_metric == "cosine") {}

SimilarityEngine::DistanceFunc SimilarityEngine::SelectDistanceFunction(const std::string& metric) {
  if (metric == "dot") {
    return vectors::DotProduct;
  }
  if (metric == "l2") {
    return [](const std::vector<float>& lhs, const std::vector<float>& rhs) -> float {
      return 1.0F / (1.0F + vectors::L2Distance(lhs, rhs));
    };
  }
  // Default: cosine
  return vectors::CosineSimilarity;
}

// ============================================================================
// Events-based Search
// ============================================================================

utils::Expected<std::vector<SimilarityResult>, utils::Error> SimilarityEngine::SearchByIdEvents(
    const std::string& item_id, int top_k) {
  auto validated_top_k = ValidateTopK(top_k);
  if (!validated_top_k) {
    return utils::MakeUnexpected(validated_top_k.error());
  }

  // Get similar items from co-occurrence index
  auto co_results = co_index_->GetSimilar(item_id, validated_top_k.value());

  // Convert to SimilarityResult
  std::vector<SimilarityResult> results;
  results.reserve(co_results.size());

  for (const auto& [item_id, score] : co_results) {
    results.emplace_back(item_id, score);
  }

  return results;
}

// ============================================================================
// Vectors-based Search
// ============================================================================

utils::Expected<std::vector<SimilarityResult>, utils::Error> SimilarityEngine::SearchByIdVectors(
    const std::string& item_id, int top_k) {
  auto validated_top_k = ValidateTopK(top_k);
  if (!validated_top_k) {
    return utils::MakeUnexpected(validated_top_k.error());
  }

  // Fast path: use compact contiguous storage for better cache locality
  if (vector_store_->IsCompactValid() && use_prenorm_) {
    auto lock = vector_store_->AcquireReadLock();
    size_t n = vector_store_->GetCompactCount();
    size_t dim = vector_store_->GetDimension();

    auto query_idx_opt = vector_store_->GetCompactIndex(item_id);
    if (!query_idx_opt) {
      return utils::MakeUnexpected(
          utils::MakeError(utils::ErrorCode::kVectorNotFound, "Query vector not found: " + item_id));
    }
    size_t query_idx = *query_idx_opt;
    const float* query_ptr = vector_store_->GetMatrixRow(query_idx);
    float query_norm = vector_store_->GetNorm(query_idx);

    // Determine whether to sample
    bool use_sampling = config_.sample_size > 0 && n > config_.sample_size * 2;

    std::vector<size_t> scan_indices;
    if (use_sampling) {
      scan_indices = SampleIndices(n, config_.sample_size);
    } else {
      scan_indices.resize(n);
      std::iota(scan_indices.begin(), scan_indices.end(), size_t(0));
    }

    std::vector<SimilarityResult> results;
    results.reserve(scan_indices.size());

    for (size_t idx : scan_indices) {
      if (idx == query_idx) {
        continue;
      }
      float score = vectors::CosineSimilarityPreNorm(query_ptr, vector_store_->GetMatrixRow(idx), dim, query_norm,
                                                     vector_store_->GetNorm(idx));
      results.emplace_back(vector_store_->GetIdByIndex(idx), score);
    }

    return MergeAndSelectTopK(std::move(results), validated_top_k.value());
  }

  // Fallback: brute-force with hash map lookups (when compact is not valid)

  // Get query vector
  auto query_vec = vector_store_->GetVector(item_id);
  if (!query_vec) {
    auto error = utils::MakeError(utils::ErrorCode::kVectorNotFound, "Query vector not found: " + item_id);
    return utils::MakeUnexpected(error);
  }

  // Get all vector IDs
  auto all_ids = vector_store_->GetAllIds();

  // Compute similarity scores for all vectors
  std::vector<SimilarityResult> results;
  results.reserve(all_ids.size());

  for (const auto& candidate_id : all_ids) {
    if (candidate_id == item_id) {
      continue;  // Skip self
    }

    auto candidate_vec = vector_store_->GetVector(candidate_id);
    if (!candidate_vec) {
      continue;  // Skip if vector not found
    }

    // Compute similarity using configured distance function
    float score = distance_func_(query_vec->data, candidate_vec->data);

    results.emplace_back(candidate_id, score);
  }

  // Sort by score descending and select top-k
  return MergeAndSelectTopK(std::move(results), validated_top_k.value());
}

// ============================================================================
// Fusion Search
// ============================================================================

utils::Expected<std::vector<SimilarityResult>, utils::Error> SimilarityEngine::SearchByIdFusion(
    const std::string& item_id, int top_k) {
  auto validated_top_k = ValidateTopK(top_k);
  if (!validated_top_k) {
    return utils::MakeUnexpected(validated_top_k.error());
  }

  // Get results from both sources (request more to ensure good coverage)
  int fetch_k = std::min(validated_top_k.value() * 3, static_cast<int>(config_.max_top_k));

  auto event_results = SearchByIdEvents(item_id, fetch_k);

  // Pre-filtering: use event results as candidate set for vector search
  utils::Expected<std::vector<SimilarityResult>, utils::Error> vector_results;
  if (event_results && event_results->size() >= static_cast<size_t>(fetch_k)) {
    // Use event results as candidate set (pre-filter)
    std::unordered_set<std::string> candidate_ids;
    for (const auto& r : *event_results) {
      candidate_ids.insert(r.item_id);
    }
    vector_results = SearchByIdVectorsFiltered(item_id, candidate_ids, fetch_k);
  } else {
    // Not enough event candidates, fall back to full vector scan
    vector_results = SearchByIdVectors(item_id, fetch_k);
  }

  // If both failed, return error
  if (!event_results && !vector_results) {
    auto error = utils::MakeError(utils::ErrorCode::kSimilaritySearchFailed,
                                  "Both event and vector searches failed for ID: " + item_id);
    return utils::MakeUnexpected(error);
  }

  // Normalize scores for each source
  if (event_results) {
    NormalizeScores(*event_results);
  }
  if (vector_results) {
    NormalizeScores(*vector_results);
  }

  // Merge scores with weights
  std::map<std::string, float> fusion_scores;

  if (event_results) {
    for (const auto& result : *event_results) {
      fusion_scores[result.item_id] += static_cast<float>(config_.fusion_beta * result.score);
    }
  }

  if (vector_results) {
    for (const auto& result : *vector_results) {
      fusion_scores[result.item_id] += static_cast<float>(config_.fusion_alpha * result.score);
    }
  }

  // Convert to result vector
  std::vector<SimilarityResult> results;
  results.reserve(fusion_scores.size());

  for (const auto& [item_id, score] : fusion_scores) {
    results.emplace_back(item_id, score);
  }

  // Sort and select top-k
  return MergeAndSelectTopK(std::move(results), validated_top_k.value());
}

// ============================================================================
// Vector Query Search (SIMV)
// ============================================================================

utils::Expected<std::vector<SimilarityResult>, utils::Error> SimilarityEngine::SearchByVector(
    const std::vector<float>& query_vector, int top_k) {
  auto validated_top_k = ValidateTopK(top_k);
  if (!validated_top_k) {
    return utils::MakeUnexpected(validated_top_k.error());
  }

  if (query_vector.empty()) {
    auto error = utils::MakeError(utils::ErrorCode::kInvalidArgument, "Query vector cannot be empty");
    return utils::MakeUnexpected(error);
  }

  // Validate dimension
  size_t expected_dim = vector_store_->GetDimension();
  if (expected_dim > 0 && query_vector.size() != expected_dim) {
    auto error = utils::MakeError(utils::ErrorCode::kVectorDimensionMismatch,
                                  "Query vector dimension mismatch: expected " + std::to_string(expected_dim) +
                                      ", got " + std::to_string(query_vector.size()));
    return utils::MakeUnexpected(error);
  }

  // Fast path: use compact contiguous storage for better cache locality
  if (vector_store_->IsCompactValid() && use_prenorm_) {
    auto lock = vector_store_->AcquireReadLock();
    size_t n = vector_store_->GetCompactCount();
    size_t dim = vector_store_->GetDimension();

    // Compute query norm
    float query_norm = 0.0F;
    for (float v : query_vector) {
      query_norm += v * v;
    }
    query_norm = std::sqrt(query_norm);

    // Determine whether to sample
    bool use_sampling = config_.sample_size > 0 && n > config_.sample_size * 2;

    std::vector<size_t> scan_indices;
    if (use_sampling) {
      scan_indices = SampleIndices(n, config_.sample_size);
    } else {
      scan_indices.resize(n);
      std::iota(scan_indices.begin(), scan_indices.end(), size_t(0));
    }

    std::vector<SimilarityResult> results;
    results.reserve(scan_indices.size());

    for (size_t idx : scan_indices) {
      float score = vectors::CosineSimilarityPreNorm(query_vector.data(), vector_store_->GetMatrixRow(idx), dim,
                                                     query_norm, vector_store_->GetNorm(idx));
      results.emplace_back(vector_store_->GetIdByIndex(idx), score);
    }

    return MergeAndSelectTopK(std::move(results), validated_top_k.value());
  }

  // Fallback: brute-force with hash map lookups (when compact is not valid)

  // Get all vector IDs
  auto all_ids = vector_store_->GetAllIds();

  // Compute similarity scores for all vectors
  std::vector<SimilarityResult> results;
  results.reserve(all_ids.size());

  for (const auto& candidate_id : all_ids) {
    auto candidate_vec = vector_store_->GetVector(candidate_id);
    if (!candidate_vec) {
      continue;  // Skip if vector not found
    }

    // Compute similarity using configured distance function
    float score = distance_func_(query_vector, candidate_vec->data);

    results.emplace_back(candidate_id, score);
  }

  // Sort by score descending and select top-k
  return MergeAndSelectTopK(std::move(results), validated_top_k.value());
}

// ============================================================================
// Sampling
// ============================================================================

std::vector<size_t> SimilarityEngine::SampleIndices(size_t total, size_t sample_size) const {
  if (sample_size >= total) {
    std::vector<size_t> all(total);
    std::iota(all.begin(), all.end(), size_t(0));
    return all;
  }

  // Reservoir sampling (Algorithm R)
  thread_local std::mt19937 rng(std::random_device{}());

  std::vector<size_t> reservoir(sample_size);
  std::iota(reservoir.begin(), reservoir.end(), size_t(0));

  for (size_t i = sample_size; i < total; ++i) {
    std::uniform_int_distribution<size_t> dist(0, i);
    size_t j = dist(rng);
    if (j < sample_size) {
      reservoir[j] = i;
    }
  }

  return reservoir;
}

// ============================================================================
// Pre-filtered Search
// ============================================================================

utils::Expected<std::vector<SimilarityResult>, utils::Error> SimilarityEngine::SearchByIdVectorsFiltered(
    const std::string& item_id, const std::unordered_set<std::string>& candidate_ids, int top_k) {
  auto validated_top_k = ValidateTopK(top_k);
  if (!validated_top_k) {
    return utils::MakeUnexpected(validated_top_k.error());
  }

  auto query_vec = vector_store_->GetVector(item_id);
  if (!query_vec) {
    return utils::MakeUnexpected(
        utils::MakeError(utils::ErrorCode::kVectorNotFound, "Query vector not found: " + item_id));
  }

  std::vector<SimilarityResult> results;
  results.reserve(candidate_ids.size());

  // Use compact path if available
  if (vector_store_->IsCompactValid() && use_prenorm_) {
    auto lock = vector_store_->AcquireReadLock();
    size_t dim = vector_store_->GetDimension();
    auto query_idx_opt = vector_store_->GetCompactIndex(item_id);
    if (!query_idx_opt) {
      return utils::MakeUnexpected(
          utils::MakeError(utils::ErrorCode::kVectorNotFound, "Query vector not found in compact: " + item_id));
    }
    const float* query_ptr = vector_store_->GetMatrixRow(*query_idx_opt);
    float query_norm = vector_store_->GetNorm(*query_idx_opt);

    for (const auto& cid : candidate_ids) {
      if (cid == item_id) {
        continue;
      }
      auto cidx = vector_store_->GetCompactIndex(cid);
      if (!cidx) {
        continue;  // candidate not in vector store
      }
      float score = vectors::CosineSimilarityPreNorm(query_ptr, vector_store_->GetMatrixRow(*cidx), dim, query_norm,
                                                     vector_store_->GetNorm(*cidx));
      results.emplace_back(cid, score);
    }
  } else {
    // Fallback: use hash map
    for (const auto& cid : candidate_ids) {
      if (cid == item_id) {
        continue;
      }
      auto cvec = vector_store_->GetVector(cid);
      if (!cvec) {
        continue;
      }
      float score = distance_func_(query_vec->data, cvec->data);
      results.emplace_back(cid, score);
    }
  }

  return MergeAndSelectTopK(std::move(results), validated_top_k.value());
}

// ============================================================================
// Helper Methods
// ============================================================================

utils::Expected<int, utils::Error> SimilarityEngine::ValidateTopK(int top_k) const {
  if (top_k <= 0) {
    auto error = utils::MakeError(utils::ErrorCode::kInvalidArgument, "top_k must be positive");
    return utils::MakeUnexpected(error);
  }

  if (top_k > static_cast<int>(config_.max_top_k)) {
    auto error = utils::MakeError(utils::ErrorCode::kInvalidArgument,
                                  "top_k exceeds maximum allowed: " + std::to_string(config_.max_top_k));
    return utils::MakeUnexpected(error);
  }

  return top_k;
}

void SimilarityEngine::NormalizeScores(std::vector<SimilarityResult>& results) {
  if (results.size() <= 1) {
    // Single result or empty: set to 0.5 to avoid undue weight in fusion
    for (auto& result : results) {
      result.score = 0.5F;
    }
    return;
  }

  // Find min and max scores
  float min_score = results[0].score;
  float max_score = results[0].score;

  for (const auto& result : results) {
    min_score = std::min(min_score, result.score);
    max_score = std::max(max_score, result.score);
  }

  // Normalize to [0, 1]
  float range = max_score - min_score;
  constexpr float kMinRange = 1e-4F;
  if (range < kMinRange) {
    // All scores are effectively the same, set to 0.5
    for (auto& result : results) {
      result.score = 0.5F;
    }
    return;
  }

  for (auto& result : results) {
    result.score = (result.score - min_score) / range;
  }
}

std::vector<SimilarityResult> SimilarityEngine::MergeAndSelectTopK(std::vector<SimilarityResult> results, int top_k) {
  if (results.size() <= static_cast<size_t>(top_k)) {
    // Sort all if fewer results than requested
    std::sort(results.begin(), results.end());
    return results;
  }

  // Partial sort for top-k (more efficient than full sort)
  std::partial_sort(results.begin(), results.begin() + top_k, results.end());
  results.resize(static_cast<size_t>(top_k));

  return results;
}

}  // namespace nvecd::similarity
