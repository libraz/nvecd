/**
 * @file similarity_engine.cpp
 * @brief Similarity engine implementation
 */

#include "similarity/similarity_engine.h"

#include <algorithm>
#include <map>
#include <unordered_set>

#include "utils/error.h"
#include "utils/structured_log.h"
#include "vectors/distance.h"

namespace nvecd::similarity {

SimilarityEngine::SimilarityEngine(events::EventStore* event_store,
                                   events::CoOccurrenceIndex* co_index,
                                   vectors::VectorStore* vector_store,
                                   const config::SimilarityConfig& config)
    : event_store_(event_store),
      co_index_(co_index),
      vector_store_(vector_store),
      config_(config) {}

// ============================================================================
// Events-based Search
// ============================================================================

utils::Expected<std::vector<SimilarityResult>, utils::Error>
SimilarityEngine::SearchByIdEvents(const std::string& id, int top_k) {
  auto validated_top_k = ValidateTopK(top_k);
  if (!validated_top_k) {
    return utils::MakeUnexpected(validated_top_k.error());
  }

  // Get similar items from co-occurrence index
  auto co_results = co_index_->GetSimilar(id, validated_top_k.value());

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

utils::Expected<std::vector<SimilarityResult>, utils::Error>
SimilarityEngine::SearchByIdVectors(const std::string& id, int top_k) {
  auto validated_top_k = ValidateTopK(top_k);
  if (!validated_top_k) {
    return utils::MakeUnexpected(validated_top_k.error());
  }

  // Get query vector
  auto query_vec = vector_store_->GetVector(id);
  if (!query_vec) {
    auto error = utils::MakeError(utils::ErrorCode::kVectorNotFound,
                                  "Query vector not found: " + id);
    return utils::MakeUnexpected(error);
  }

  // Get all vector IDs
  auto all_ids = vector_store_->GetAllIds();

  // Compute similarity scores for all vectors
  std::vector<SimilarityResult> results;
  results.reserve(all_ids.size());

  for (const auto& candidate_id : all_ids) {
    if (candidate_id == id) {
      continue;  // Skip self
    }

    auto candidate_vec = vector_store_->GetVector(candidate_id);
    if (!candidate_vec) {
      continue;  // Skip if vector not found
    }

    // Compute similarity (using cosine similarity by default)
    float score =
        vectors::CosineSimilarity(query_vec->data, candidate_vec->data);

    results.emplace_back(candidate_id, score);
  }

  // Sort by score descending and select top-k
  return MergeAndSelectTopK(std::move(results), validated_top_k.value());
}

// ============================================================================
// Fusion Search
// ============================================================================

utils::Expected<std::vector<SimilarityResult>, utils::Error>
SimilarityEngine::SearchByIdFusion(const std::string& id, int top_k) {
  auto validated_top_k = ValidateTopK(top_k);
  if (!validated_top_k) {
    return utils::MakeUnexpected(validated_top_k.error());
  }

  // Get results from both sources (request more to ensure good coverage)
  int fetch_k = std::min(validated_top_k.value() * 3,
                         static_cast<int>(config_.max_top_k));

  auto event_results = SearchByIdEvents(id, fetch_k);
  auto vector_results = SearchByIdVectors(id, fetch_k);

  // If both failed, return error
  if (!event_results && !vector_results) {
    auto error = utils::MakeError(
        utils::ErrorCode::kSimilaritySearchFailed,
        "Both event and vector searches failed for ID: " + id);
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
      fusion_scores[result.id] += config_.fusion_beta * result.score;
    }
  }

  if (vector_results) {
    for (const auto& result : *vector_results) {
      fusion_scores[result.id] += config_.fusion_alpha * result.score;
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

utils::Expected<std::vector<SimilarityResult>, utils::Error>
SimilarityEngine::SearchByVector(const std::vector<float>& query_vector,
                                 int top_k) {
  auto validated_top_k = ValidateTopK(top_k);
  if (!validated_top_k) {
    return utils::MakeUnexpected(validated_top_k.error());
  }

  if (query_vector.empty()) {
    auto error = utils::MakeError(utils::ErrorCode::kInvalidArgument,
                                  "Query vector cannot be empty");
    return utils::MakeUnexpected(error);
  }

  // Validate dimension
  size_t expected_dim = vector_store_->GetDimension();
  if (expected_dim > 0 && query_vector.size() != expected_dim) {
    auto error = utils::MakeError(
        utils::ErrorCode::kVectorDimensionMismatch,
        "Query vector dimension mismatch: expected " +
            std::to_string(expected_dim) + ", got " +
            std::to_string(query_vector.size()));
    return utils::MakeUnexpected(error);
  }

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

    // Compute similarity (using cosine similarity by default)
    float score = vectors::CosineSimilarity(query_vector, candidate_vec->data);

    results.emplace_back(candidate_id, score);
  }

  // Sort by score descending and select top-k
  return MergeAndSelectTopK(std::move(results), validated_top_k.value());
}

// ============================================================================
// Helper Methods
// ============================================================================

utils::Expected<int, utils::Error> SimilarityEngine::ValidateTopK(
    int top_k) const {
  if (top_k <= 0) {
    auto error = utils::MakeError(utils::ErrorCode::kInvalidArgument,
                                  "top_k must be positive");
    return utils::MakeUnexpected(error);
  }

  if (top_k > static_cast<int>(config_.max_top_k)) {
    auto error = utils::MakeError(
        utils::ErrorCode::kInvalidArgument,
        "top_k exceeds maximum allowed: " + std::to_string(config_.max_top_k));
    return utils::MakeUnexpected(error);
  }

  return top_k;
}

void SimilarityEngine::NormalizeScores(
    std::vector<SimilarityResult>& results) const {
  if (results.empty()) {
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
  if (range < 1e-6f) {
    // All scores are the same, set to 1.0
    for (auto& result : results) {
      result.score = 1.0f;
    }
    return;
  }

  for (auto& result : results) {
    result.score = (result.score - min_score) / range;
  }
}

std::vector<SimilarityResult> SimilarityEngine::MergeAndSelectTopK(
    std::vector<SimilarityResult> results, int top_k) const {
  // Sort by score descending
  std::sort(results.begin(), results.end());

  // Select top-k
  if (results.size() > static_cast<size_t>(top_k)) {
    results.resize(static_cast<size_t>(top_k));
  }

  return results;
}

}  // namespace nvecd::similarity
