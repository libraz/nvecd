/**
 * @file similarity_engine.cpp
 * @brief Similarity engine implementation
 */

#include "similarity/similarity_engine.h"

#include <algorithm>
#include <map>
#include <numeric>
#include <queue>
#include <random>
#include <unordered_set>

#include "utils/error.h"
#include "utils/structured_log.h"
#include "vectors/distance.h"
#include "vectors/ivf_index.h"

namespace nvecd::similarity {

SimilarityEngine::SimilarityEngine(events::EventStore* event_store, events::CoOccurrenceIndex* co_index,
                                   vectors::VectorStore* vector_store, const config::SimilarityConfig& config,
                                   const config::VectorsConfig& vectors_config)
    : event_store_(event_store),
      co_index_(co_index),
      vector_store_(vector_store),
      config_(config),
      distance_func_(SelectDistanceFunction(vectors_config.distance_metric)),
      use_prenorm_(vectors_config.distance_metric.empty() || vectors_config.distance_metric == "cosine") {
  if (config_.ivf_enabled) {
    vectors::IvfIndex::Config ivf_config;
    ivf_config.nlist = config_.ivf_nlist;
    ivf_config.nprobe = config_.ivf_nprobe;
    ivf_config.train_threshold = config_.ivf_train_threshold;
    ivf_index_ = std::make_unique<vectors::IvfIndex>(
        vectors_config.default_dimension, ivf_config);
    utils::StructuredLog()
        .Event("ivf_index_created")
        .Field("nlist", static_cast<int64_t>(config_.ivf_nlist))
        .Field("nprobe", static_cast<int64_t>(config_.ivf_nprobe))
        .Field("train_threshold", static_cast<int64_t>(config_.ivf_train_threshold))
        .Info();
  }
}

SimilarityEngine::~SimilarityEngine() { JoinTrainThread(); }

void SimilarityEngine::JoinTrainThread() {
  if (ivf_train_thread_ && ivf_train_thread_->joinable()) {
    ivf_train_thread_->join();
  }
  ivf_train_thread_.reset();
}

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

  auto snap = vector_store_->GetCompactSnapshot();
  if (snap.count == 0 || snap.matrix == nullptr) {
    // Empty store: check if the query ID exists (it won't in an empty store)
    return utils::MakeUnexpected(
        utils::MakeError(utils::ErrorCode::kVectorNotFound, "Query vector not found: " + item_id));
  }

  auto query_it = snap.id_to_idx->find(item_id);
  if (query_it == snap.id_to_idx->end()) {
    return utils::MakeUnexpected(
        utils::MakeError(utils::ErrorCode::kVectorNotFound, "Query vector not found: " + item_id));
  }
  size_t query_idx = query_it->second;
  const float* query_ptr = snap.matrix + query_idx * snap.dim;
  float query_norm = snap.norms[query_idx];

  // IVF accelerated path: use IVF index if enabled and trained
  if (ivf_index_ && ivf_index_->IsTrained() && use_prenorm_) {
    auto ivf_results = ivf_index_->Search(
        query_ptr, query_norm, snap.matrix, snap.norms,
        snap.count, static_cast<uint32_t>(snap.dim),
        static_cast<size_t>(validated_top_k.value()) + 1);  // +1 to exclude self

    std::vector<SimilarityResult> results;
    results.reserve(ivf_results.size());
    for (const auto& [score, idx] : ivf_results) {
      if (idx == query_idx) {
        continue;  // Exclude self
      }
      if (vector_store_->IsDeleted(idx)) {
        continue;
      }
      results.emplace_back((*snap.idx_to_id)[idx], score);
      if (static_cast<int>(results.size()) >= validated_top_k.value()) {
        break;
      }
    }
    std::sort(results.begin(), results.end());
    return results;
  }

  // For non-cosine metrics, build query vector once
  std::vector<float> query_vec_data;
  if (!use_prenorm_) {
    query_vec_data.assign(query_ptr, query_ptr + snap.dim);
  }

  // Bounded min-heap: track top-k (score, index) pairs without string allocation
  int k = validated_top_k.value();
  using ScoreIdx = std::pair<float, size_t>;
  auto cmp = [](const ScoreIdx& a, const ScoreIdx& b) { return a.first > b.first; };
  std::priority_queue<ScoreIdx, std::vector<ScoreIdx>, decltype(cmp)> min_heap(cmp);

  // Lambda to push a candidate into the min-heap
  auto push_candidate = [&](size_t idx) {
    if (idx == query_idx) {
      return;
    }
    if (vector_store_->IsDeleted(idx)) {
      return;
    }
    float score;
    if (use_prenorm_) {
      score = vectors::CosineSimilarityPreNorm(query_ptr, snap.matrix + idx * snap.dim, snap.dim, query_norm,
                                               snap.norms[idx]);
    } else {
      const float* cand_ptr = snap.matrix + idx * snap.dim;
      std::vector<float> cand_data(cand_ptr, cand_ptr + snap.dim);
      score = distance_func_(query_vec_data, cand_data);
    }
    if (static_cast<int>(min_heap.size()) < k) {
      min_heap.push({score, idx});
    } else if (score > min_heap.top().first) {
      min_heap.pop();
      min_heap.push({score, idx});
    }
  };

  // Determine whether to sample; avoid allocating full index vector for full scan
  bool use_sampling = config_.sample_size > 0 && snap.count > config_.sample_size * 2;
  if (use_sampling) {
    auto scan_indices = SampleIndices(snap.count, config_.sample_size);
    for (size_t idx : scan_indices) {
      push_candidate(idx);
    }
  } else {
    // Full scan with prefetching to hide memory latency
    constexpr size_t kPrefetchAhead = 4;
    for (size_t idx = 0; idx < snap.count; ++idx) {
      // Prefetch upcoming vector data into L1 cache
      if (idx + kPrefetchAhead < snap.count) {
        __builtin_prefetch(snap.matrix + (idx + kPrefetchAhead) * snap.dim, 0, 0);
      }
      push_candidate(idx);
    }
  }

  // Convert heap to sorted results (only top-k strings allocated)
  std::vector<SimilarityResult> results;
  results.reserve(min_heap.size());
  while (!min_heap.empty()) {
    auto [s, i] = min_heap.top();
    min_heap.pop();
    results.emplace_back((*snap.idx_to_id)[i], s);
  }
  std::sort(results.begin(), results.end());  // SimilarityResult sorts by score desc
  return results;
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

  auto snap = vector_store_->GetCompactSnapshot();
  if (snap.count == 0 || snap.matrix == nullptr) {
    return std::vector<SimilarityResult>{};
  }

  // Compute query norm using SIMD
  float query_norm = vectors::simd::GetOptimalImpl().l2_norm(query_vector.data(), query_vector.size());

  // IVF accelerated path: use IVF index if enabled and trained
  if (ivf_index_ && ivf_index_->IsTrained() && use_prenorm_) {
    auto ivf_results = ivf_index_->Search(
        query_vector.data(), query_norm, snap.matrix, snap.norms,
        snap.count, static_cast<uint32_t>(snap.dim),
        static_cast<size_t>(validated_top_k.value()));

    std::vector<SimilarityResult> results;
    results.reserve(ivf_results.size());
    for (const auto& [score, idx] : ivf_results) {
      if (vector_store_->IsDeleted(idx)) {
        continue;
      }
      results.emplace_back((*snap.idx_to_id)[idx], score);
    }
    std::sort(results.begin(), results.end());
    return results;
  }

  // Bounded min-heap: track top-k (score, index) pairs without string allocation
  int k = validated_top_k.value();
  using ScoreIdx = std::pair<float, size_t>;
  auto cmp = [](const ScoreIdx& a, const ScoreIdx& b) { return a.first > b.first; };
  std::priority_queue<ScoreIdx, std::vector<ScoreIdx>, decltype(cmp)> min_heap(cmp);

  // Lambda to push a candidate into the min-heap
  auto push_candidate = [&](size_t idx) {
    if (vector_store_->IsDeleted(idx)) {
      return;
    }
    float score;
    if (use_prenorm_) {
      score = vectors::CosineSimilarityPreNorm(query_vector.data(), snap.matrix + idx * snap.dim, snap.dim,
                                               query_norm, snap.norms[idx]);
    } else {
      const float* cand_ptr = snap.matrix + idx * snap.dim;
      std::vector<float> cand_data(cand_ptr, cand_ptr + snap.dim);
      score = distance_func_(query_vector, cand_data);
    }
    if (static_cast<int>(min_heap.size()) < k) {
      min_heap.push({score, idx});
    } else if (score > min_heap.top().first) {
      min_heap.pop();
      min_heap.push({score, idx});
    }
  };

  // Determine whether to sample; avoid allocating full index vector for full scan
  bool use_sampling = config_.sample_size > 0 && snap.count > config_.sample_size * 2;
  if (use_sampling) {
    auto scan_indices = SampleIndices(snap.count, config_.sample_size);
    for (size_t idx : scan_indices) {
      push_candidate(idx);
    }
  } else {
    // Full scan with prefetching to hide memory latency
    constexpr size_t kPrefetchAhead = 4;
    for (size_t idx = 0; idx < snap.count; ++idx) {
      if (idx + kPrefetchAhead < snap.count) {
        __builtin_prefetch(snap.matrix + (idx + kPrefetchAhead) * snap.dim, 0, 0);
      }
      push_candidate(idx);
    }
  }

  // Convert heap to sorted results (only top-k strings allocated)
  std::vector<SimilarityResult> results;
  results.reserve(min_heap.size());
  while (!min_heap.empty()) {
    auto [s, i] = min_heap.top();
    min_heap.pop();
    results.emplace_back((*snap.idx_to_id)[i], s);
  }
  std::sort(results.begin(), results.end());  // SimilarityResult sorts by score desc
  return results;
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

  // Reservoir sampling with fast modulo-based random (avoids creating
  // uniform_int_distribution per iteration which is the main bottleneck)
  thread_local std::mt19937 rng(std::random_device{}());

  std::vector<size_t> reservoir(sample_size);
  std::iota(reservoir.begin(), reservoir.end(), size_t(0));

  for (size_t i = sample_size; i < total; ++i) {
    // Fast bounded random: rng() % (i+1) has slight bias but is much faster
    // than creating uniform_int_distribution per iteration. The bias is
    // negligible for large i values typical in vector search workloads.
    size_t j = rng() % (i + 1);
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

  if (vector_store_->GetVectorCount() > 0) {
    auto snap = vector_store_->GetCompactSnapshot();
    if (snap.count == 0 || snap.matrix == nullptr) {
      return std::vector<SimilarityResult>{};
    }

    auto query_it = snap.id_to_idx->find(item_id);
    if (query_it == snap.id_to_idx->end()) {
      return utils::MakeUnexpected(
          utils::MakeError(utils::ErrorCode::kVectorNotFound, "Query vector not found in compact: " + item_id));
    }
    size_t query_idx = query_it->second;
    const float* query_ptr = snap.matrix + query_idx * snap.dim;
    float query_norm = snap.norms[query_idx];

    // For non-cosine metrics, build query vector once
    std::vector<float> query_vec_data;
    if (!use_prenorm_) {
      query_vec_data.assign(query_ptr, query_ptr + snap.dim);
    }

    for (const auto& cid : candidate_ids) {
      if (cid == item_id) {
        continue;
      }
      auto cidx_it = snap.id_to_idx->find(cid);
      if (cidx_it == snap.id_to_idx->end()) {
        continue;  // candidate not in vector store
      }
      size_t cidx = cidx_it->second;
      if (vector_store_->IsDeleted(cidx)) {
        continue;
      }
      float score;
      if (use_prenorm_) {
        score = vectors::CosineSimilarityPreNorm(query_ptr, snap.matrix + cidx * snap.dim, snap.dim, query_norm,
                                                 snap.norms[cidx]);
      } else {
        const float* cand_ptr = snap.matrix + cidx * snap.dim;
        std::vector<float> cand_data(cand_ptr, cand_ptr + snap.dim);
        score = distance_func_(query_vec_data, cand_data);
      }
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

// ============================================================================
// IVF Index Integration
// ============================================================================

void SimilarityEngine::NotifyVectorAdded(size_t compact_index, const float* vector) {
  if (!ivf_index_) {
    return;
  }

  if (ivf_index_->IsTrained()) {
    ivf_index_->AddVector(compact_index, vector);

    // No automatic re-training during AddVector.
    // Re-training would be triggered by explicit command or scheduled task.
  } else {
    MaybeTrainIvfIndex();
  }
}

void SimilarityEngine::NotifyVectorRemoved(size_t compact_index) {
  if (!ivf_index_ || !ivf_index_->IsTrained()) {
    return;
  }
  ivf_index_->RemoveVector(compact_index);
}

bool SimilarityEngine::IsIvfTrained() const {
  return ivf_index_ && ivf_index_->IsTrained();
}

void SimilarityEngine::ForceIvfTrain() {
  if (!ivf_index_) {
    return;
  }
  // Reset trained state to allow re-training with current data size
  if (ivf_index_->IsTrained()) {
    ivf_index_->ResetTrained();
  }
  MaybeTrainIvfIndex();
}

void SimilarityEngine::MaybeTrainIvfIndex() {
  if (!ivf_index_ || ivf_index_->IsTrained()) {
    return;
  }

  // Prevent concurrent training launches
  bool expected = false;
  if (!ivf_training_.compare_exchange_strong(expected, true)) {
    return;  // Training already in progress
  }

  size_t vec_count = vector_store_->GetVectorCount();
  if (vec_count < config_.ivf_train_threshold) {
    ivf_training_.store(false);
    return;
  }

  // Take a snapshot and copy only a sample of vectors for k-means training.
  // After centroids are learned, all existing vectors are assigned via AddVector
  // in a second pass (cheaper than copying the entire matrix).
  auto snap = vector_store_->GetCompactSnapshot();
  if (snap.count == 0 || snap.matrix == nullptr) {
    ivf_training_.store(false);
    return;
  }

  uint32_t dim = static_cast<uint32_t>(snap.dim);

  // Build valid indices array (non-deleted entries)
  std::vector<size_t> valid_indices;
  valid_indices.reserve(snap.count);
  for (size_t i = 0; i < snap.count; ++i) {
    if (!vector_store_->IsDeleted(i)) {
      valid_indices.push_back(i);
    }
  }

  if (valid_indices.empty()) {
    ivf_training_.store(false);
    return;
  }

  // Copy only the vectors we need for training (sample up to 10K).
  // IvfIndex::Train internally subsamples, but we copy the full set of
  // valid vectors up to a cap to keep memory usage bounded.
  constexpr size_t kMaxCopyForTrain = 50000;
  std::vector<size_t> train_indices;
  if (valid_indices.size() <= kMaxCopyForTrain) {
    train_indices = valid_indices;
  } else {
    // Reservoir sample
    train_indices.resize(kMaxCopyForTrain);
    std::copy(valid_indices.begin(), valid_indices.begin() + kMaxCopyForTrain,
              train_indices.begin());
    std::mt19937 rng(42);
    for (size_t i = kMaxCopyForTrain; i < valid_indices.size(); ++i) {
      size_t j = rng() % (i + 1);
      if (j < kMaxCopyForTrain) {
        train_indices[j] = valid_indices[i];
      }
    }
  }

  // Copy sample vectors into a compact matrix (contiguous, re-indexed 0..N-1)
  auto sample_matrix = std::make_shared<std::vector<float>>(train_indices.size() * dim);
  std::vector<size_t> sample_indices(train_indices.size());
  for (size_t i = 0; i < train_indices.size(); ++i) {
    size_t src_idx = train_indices[i];
    std::copy(snap.matrix + src_idx * dim,
              snap.matrix + (src_idx + 1) * dim,
              sample_matrix->data() + i * dim);
    sample_indices[i] = i;  // Re-index to [0, N)
  }

  // Save valid_indices for post-training bulk assignment
  auto all_valid = std::make_shared<std::vector<size_t>>(std::move(valid_indices));

  utils::StructuredLog()
      .Event("ivf_training_start_async")
      .Field("vector_count", static_cast<int64_t>(all_valid->size()))
      .Field("sample_size", static_cast<int64_t>(train_indices.size()))
      .Info();

  // Join any previous training thread before launching a new one
  JoinTrainThread();

  // Launch training in a background thread.
  // Phase 2 reads VectorStore matrix directly under a brief read lock
  // rather than copying the entire matrix (saves ~1.2GB at 10M scale).
  auto* vs = vector_store_;
  ivf_train_thread_ = std::make_unique<std::thread>(
      [this, sample_matrix, sample_indices = std::move(sample_indices),
       all_valid, dim, vs]() {
        // Phase 1: Train centroids on the sample (no vector assignment)
        ivf_index_->Train(sample_matrix->data(), sample_indices.data(),
                          sample_indices.size(), dim, false);

        // Phase 2: Bulk-assign all vectors using VectorStore matrix directly.
        // Hold read lock for the duration to ensure matrix pointer validity.
        {
          auto lock = vs->AcquireReadLock();
          auto snap2 = vs->GetCompactSnapshot();
          if (snap2.matrix != nullptr && snap2.count > 0) {
            for (size_t idx : *all_valid) {
              if (idx < snap2.count) {
                ivf_index_->AddVector(idx, snap2.matrix + idx * dim);
              }
            }
          }
        }

        utils::StructuredLog()
            .Event("ivf_training_complete_async")
            .Field("trained", ivf_index_->IsTrained())
            .Field("indexed_count", static_cast<int64_t>(ivf_index_->GetIndexedCount()))
            .Info();

        ivf_training_.store(false);
      });
}

}  // namespace nvecd::similarity
