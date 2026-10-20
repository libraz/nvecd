/**
 * @file ivf_index.cpp
 * @brief IVF (Inverted File) index implementation
 *
 * K-means training uses Lloyd's algorithm with random initialization.
 * Search uses bounded min-heap for top-k selection.
 * Two-tier architecture: write buffer (brute-force) + sealed IVF index.
 */

#include "vectors/ivf_index.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <queue>
#include <random>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "utils/random_sampling.h"
#include "utils/structured_log.h"
#include "vectors/distance.h"
#include "vectors/distance_simd.h"

namespace nvecd::vectors {

namespace {

/// Prefetch ahead distance for search loop
constexpr size_t kPrefetchAhead = 4;

/// cluster_of_ value for a compact index that is in no inverted list
constexpr uint32_t kNoCluster = std::numeric_limits<uint32_t>::max();

/// Sentinel "worse than any real score" for max-selection across all metrics
constexpr float kWorstScore = -std::numeric_limits<float>::infinity();

// ---------------------------------------------------------------------------
// Transient tier maintenance
//
// The write buffer and the sealing tier are each a trio of parallel arrays
// (indices, vectors, norms) plus a compact_index -> slot map. A norm is only
// meaningful for cosine; other metrics store 0. The three helpers below are the
// only places that mutate a tier, so the map cannot fall out of step with the
// arrays it indexes.
// Every one of them requires buffer_mutex_ held exclusively.
// ---------------------------------------------------------------------------

/// Append an entry and record its slot. The caller has already established that
/// @p compact_index is absent from this tier.
void PushToTier(size_t compact_index, const float* vector, float norm, uint32_t dim, std::vector<size_t>& indices,
                std::vector<float>& vectors, std::vector<float>& norms, std::unordered_map<size_t, size_t>& slots) {
  slots.emplace(compact_index, indices.size());
  indices.push_back(compact_index);
  vectors.insert(vectors.end(), vector, vector + dim);
  norms.push_back(norm);
}

/// Remove an entry by moving the last one into its slot. No-op when absent.
void EraseFromTier(size_t compact_index, uint32_t dim, std::vector<size_t>& indices, std::vector<float>& vectors,
                   std::vector<float>& norms, std::unordered_map<size_t, size_t>& slots) {
  auto it = slots.find(compact_index);
  if (it == slots.end()) {
    return;
  }
  const size_t slot = it->second;
  const size_t last = indices.size() - 1;
  if (slot != last) {
    indices[slot] = indices[last];
    norms[slot] = norms[last];
    std::copy(vectors.data() + last * dim, vectors.data() + (last + 1) * dim, vectors.data() + slot * dim);
    // The moved entry lives somewhere else now; its slot has to follow it.
    slots[indices[slot]] = slot;
  }
  slots.erase(compact_index);
  indices.pop_back();
  norms.pop_back();
  vectors.resize(indices.size() * dim);
}

/// Empty a tier and its slot map together.
void ClearTier(std::vector<size_t>& indices, std::vector<float>& vectors, std::vector<float>& norms,
               std::unordered_map<size_t, size_t>& slots) {
  indices.clear();
  vectors.clear();
  norms.clear();
  slots.clear();
}

}  // namespace

IvfMetric IvfMetricFromString(const std::string& metric) {
  if (metric == "dot") {
    return IvfMetric::kDot;
  }
  if (metric == "l2") {
    return IvfMetric::kL2;
  }
  return IvfMetric::kCosine;
}

IvfIndex::IvfIndex(uint32_t dimension) : config_(), configured_nlist_(config_.nlist), dimension_(dimension) {}

IvfIndex::IvfIndex(uint32_t dimension, const Config& config)
    : config_(config), configured_nlist_(config.nlist), dimension_(dimension) {}

float IvfIndex::SimilarityFor(IvfMetric metric, const float* a, float norm_a, const float* b, float norm_b,
                              uint32_t dim) {
  switch (metric) {
    case IvfMetric::kDot:
      return DotProductRaw(a, b, dim);
    case IvfMetric::kL2:
      // Monotonic decreasing in distance, so larger score == nearer.
      return L2SimilarityRaw(a, b, dim);
    case IvfMetric::kCosine:
    default:
      if (norm_a < kNormEpsilon || norm_b < kNormEpsilon) {
        return kWorstScore;  // Cosine undefined for zero-norm vectors
      }
      return DotProductRaw(a, b, dim) / (norm_a * norm_b);
  }
}

float IvfIndex::Similarity(const float* a, float norm_a, const float* b, float norm_b, uint32_t dim) const {
  return SimilarityFor(config_.metric, a, norm_a, b, norm_b, dim);
}

size_t IvfIndex::AssignCluster(IvfMetric metric, const float* centroids, const float* centroid_norms, uint32_t nlist,
                               const float* vec, float vec_norm, size_t compact_index, uint32_t dim) {
  size_t best = 0;
  float best_sim = kWorstScore;

  for (uint32_t c = 0; c < nlist; ++c) {
    const float sim =
        SimilarityFor(metric, vec, vec_norm, centroids + static_cast<size_t>(c) * dim, centroid_norms[c], dim);
    if (sim > best_sim) {
      best_sim = sim;
      best = c;
    }
  }

  // Zero-norm vectors have no meaningful nearest centroid under cosine
  // (every cluster scores -inf). Spread them deterministically instead of
  // dumping them all into cluster 0, which would skew recall.
  if (best_sim == kWorstScore) {
    best = compact_index % nlist;
  }
  return best;
}

float IvfIndex::ScoreCandidate(const float* a, float norm_a, const float* b, float norm_b, uint32_t dim) const {
  const float score = Similarity(a, norm_a, b, norm_b, dim);
  return score == kWorstScore ? 0.0F : score;
}

void IvfIndex::Train(const float* matrix, const size_t* valid_indices, size_t num_valid, uint32_t dimension,
                     bool assign_vectors, size_t corpus_size) {
  // Prevent concurrent training attempts
  bool expected = false;
  if (!training_in_progress_.compare_exchange_strong(expected, true)) {
    return;  // Another thread is already training
  }

  // Reset the training flag and stop recording changes on every exit path.
  struct TrainingGuard {
    IvfIndex& index;
    ~TrainingGuard() {
      {
        std::unique_lock lock(index.mutex_);
        index.recording_changes_ = false;
        index.change_log_.clear();
      }
      index.training_in_progress_.store(false);
    }
  } guard{*this};

  if (num_valid == 0 || matrix == nullptr || valid_indices == nullptr) {
    return;
  }

  // Snapshot the settings under the lock, then train without it: searches and
  // writes keep running against the current layout. Changes that land meanwhile
  // are recorded and replayed once the new layout is installed.
  uint32_t configured_nlist = 0;
  uint32_t max_iterations = 0;
  float convergence_threshold = 0.0F;
  IvfMetric metric = IvfMetric::kCosine;
  uint64_t start_generation = 0;
  {
    std::unique_lock lock(mutex_);
    configured_nlist = configured_nlist_;
    max_iterations = config_.max_iterations;
    convergence_threshold = config_.convergence_threshold;
    metric = config_.metric;
    start_generation = layout_generation_;
    // Set before recording so a first-run AddVector can copy its vector.
    dimension_.store(dimension, std::memory_order_relaxed);
    change_log_.clear();
    recording_changes_ = true;
  }

  // Derive the effective nlist from the operator-configured value on every run
  // rather than from whatever the previous run left in config_.nlist, so an
  // explicit setting keeps applying and the auto sentinel keeps auto-scaling.
  uint32_t effective_nlist = configured_nlist;

  // Auto-scale nlist if set to 0: use sqrt(n), capped at kMaxAutoNlist
  if (effective_nlist == 0) {
    const size_t scaled_count = corpus_size > 0 ? corpus_size : num_valid;
    auto sqrt_n = static_cast<uint32_t>(std::max(1.0, std::sqrt(static_cast<double>(scaled_count))));
    effective_nlist = std::min(sqrt_n, kMaxAutoNlist);
  }

  // Clamp nlist to not exceed the number of vectors
  if (effective_nlist > num_valid) {
    effective_nlist = static_cast<uint32_t>(num_valid);
  }

  // Subsample if too many vectors for training
  std::vector<size_t> train_indices;
  const size_t* train_ptr = valid_indices;
  size_t train_size = num_valid;

  if (num_valid > kMaxTrainingSamples) {
    // Sample positions from the live-row index list so tombstones stay excluded.
    thread_local std::mt19937 rng(std::random_device{}());
    const auto sample_positions = utils::SampleIndices(num_valid, kMaxTrainingSamples, rng);
    train_indices.reserve(kMaxTrainingSamples);
    for (size_t position : sample_positions) {
      train_indices.push_back(valid_indices[position]);
    }

    train_ptr = train_indices.data();
    train_size = kMaxTrainingSamples;
  }

  // Re-clamp against the subsampled set: centroid seeding draws one distinct
  // training sample per cluster, so an nlist above the sample count would index
  // past the seeding permutation and read an arbitrary row of the matrix.
  if (effective_nlist > train_size) {
    effective_nlist = static_cast<uint32_t>(train_size);
  }

  std::vector<float> centroids;
  std::vector<float> centroid_norms;
  KMeansTrain(matrix, train_ptr, train_size, dimension, effective_nlist, metric, max_iterations, convergence_threshold,
              centroids, centroid_norms);

  if (train_hook_) {
    train_hook_();
  }

  size_t total_indexed = 0;
  uint32_t nprobe = 0;
  {
    std::unique_lock lock(mutex_);
    recording_changes_ = false;
    if (layout_generation_ != start_generation) {
      // A reset or reconfiguration replaced the index while this run was
      // training; its centroids describe a layout that no longer exists.
      utils::StructuredLog().Event("ivf_training_discarded").Info();
      return;
    }

    dimension_.store(dimension, std::memory_order_relaxed);
    config_.nlist = effective_nlist;
    centroids_ = std::move(centroids);
    centroid_norms_ = std::move(centroid_norms);
    inverted_lists_.clear();
    inverted_lists_.resize(config_.nlist);
    cluster_of_.clear();

    const auto& simd_impl = simd::GetOptimalImpl();
    const uint32_t nlist = config_.nlist;
    const auto assign = [&](size_t idx, const float* vec) {
      const float vec_norm = simd_impl.l2_norm(vec, dimension);
      AppendToListLocked(AssignCluster(config_.metric, centroids_.data(), centroid_norms_.data(), nlist, vec, vec_norm,
                                       idx, dimension),
                         idx);
    };

    // Assign vectors to clusters if requested
    if (assign_vectors) {
      for (size_t i = 0; i < num_valid; ++i) {
        const size_t idx = valid_indices[i];
        assign(idx, matrix + idx * dimension);
      }
    }

    // Replay what changed while training, in order, so vectors added or removed
    // meanwhile end up reflected in the new layout.
    for (const auto& change : change_log_) {
      if (change.removed) {
        EraseIndexedLocked(change.compact_index);
      } else if (change.vector.size() == dimension) {
        assign(change.compact_index, change.vector.data());
      }
    }
    change_log_.clear();

    trained_ = true;
    ++layout_generation_;
    for (const auto& list : inverted_lists_) {
      total_indexed += list.size();
    }
    nprobe = config_.nprobe;
  }

  utils::StructuredLog()
      .Event("ivf_index_trained")
      .Field("nlist", static_cast<int64_t>(effective_nlist))
      .Field("nprobe", static_cast<int64_t>(nprobe))
      .Field("indexed_count", static_cast<int64_t>(total_indexed))
      .Field("dimension", static_cast<int64_t>(dimension))
      .Info();
}

void IvfIndex::AddVector(size_t compact_index, const float* vector) {
  std::unique_lock lock(mutex_);

  if (vector == nullptr) {
    return;
  }
  if (recording_changes_) {
    change_log_.push_back({compact_index, false, std::vector<float>(vector, vector + GetDimension())});
  }
  if (!trained_) {
    return;
  }

  size_t cluster = FindNearestCentroid(vector);
  AppendToListLocked(cluster, compact_index);
}

void IvfIndex::BulkAddVectors(const size_t* compact_indices, const float* vectors, size_t count, uint32_t dimension) {
  std::unique_lock lock(mutex_);

  if (compact_indices == nullptr || vectors == nullptr || count == 0) {
    return;
  }
  if (recording_changes_) {
    for (size_t i = 0; i < count; ++i) {
      const float* vec = vectors + i * dimension;
      change_log_.push_back({compact_indices[i], false, std::vector<float>(vec, vec + dimension)});
    }
  }
  if (!trained_) {
    return;
  }

  const auto& simd_impl = simd::GetOptimalImpl();
  const uint32_t nlist = config_.nlist;

  for (size_t i = 0; i < count; ++i) {
    size_t idx = compact_indices[i];
    const float* vec = vectors + i * dimension;  // Row i of the contiguous array
    float vec_norm = simd_impl.l2_norm(vec, dimension);
    AppendToListLocked(
        AssignCluster(config_.metric, centroids_.data(), centroid_norms_.data(), nlist, vec, vec_norm, idx, dimension),
        idx);
  }
}

void IvfIndex::AppendToListLocked(size_t cluster, size_t compact_index) {
  EraseIndexedLocked(compact_index);
  inverted_lists_[cluster].push_back(compact_index);
  if (compact_index >= cluster_of_.size()) {
    cluster_of_.resize(compact_index + 1, kNoCluster);
  }
  cluster_of_[compact_index] = static_cast<uint32_t>(cluster);
}

void IvfIndex::EraseIndexedLocked(size_t compact_index) {
  if (compact_index >= cluster_of_.size() || cluster_of_[compact_index] == kNoCluster) {
    return;
  }
  auto& list = inverted_lists_[cluster_of_[compact_index]];
  list.erase(std::remove(list.begin(), list.end(), compact_index), list.end());
  cluster_of_[compact_index] = kNoCluster;
}

void IvfIndex::RemoveVector(size_t compact_index) {
  // Remove from both searchable transient tiers. A vector may exist in both
  // when it is updated while an older embedding is being sealed.
  {
    std::unique_lock buf_lock(buffer_mutex_);
    const uint32_t dim = dimension_.load(std::memory_order_relaxed);
    EraseFromTier(compact_index, dim, buffer_indices_, buffer_vectors_, buffer_norms_, buffer_slots_);
    EraseFromTier(compact_index, dim, sealing_indices_, sealing_vectors_, sealing_norms_, sealing_slots_);
  }

  // Then the inverted list. Most calls are the retire step of a first insert,
  // so the exclusive lock is taken only when the index is actually sealed.
  {
    std::shared_lock lock(mutex_);
    if (!recording_changes_ && (compact_index >= cluster_of_.size() || cluster_of_[compact_index] == kNoCluster)) {
      return;
    }
  }
  std::unique_lock lock(mutex_);
  if (recording_changes_) {
    change_log_.push_back({compact_index, true, {}});
  }
  EraseIndexedLocked(compact_index);
}

std::vector<std::pair<float, size_t>> IvfIndex::Search(const float* query_vec, float query_norm, const float* matrix,
                                                       const float* norms, size_t total_count, uint32_t dimension,
                                                       size_t top_k) const {
  if (top_k == 0) {
    return {};
  }
  // Both tiers were written at dimension_; scanning them at a different stride
  // would read past the buffers and score against misaligned rows. A mismatch
  // means the index was not rebound to the store's dimension, so there is no
  // correct result to return.
  if (dimension != dimension_.load(std::memory_order_relaxed)) {
    return {};
  }

  // Search the write buffer first (independent lock)
  auto buffer_results = SearchBuffer(query_vec, query_norm, dimension, top_k);

  // Search the IVF inverted lists
  std::vector<std::pair<float, size_t>> ivf_results;
  {
    std::shared_lock lock(mutex_);

    if (trained_ && query_vec != nullptr && matrix != nullptr && norms != nullptr && total_count > 0) {
      // Find nprobe nearest centroids
      uint32_t nprobe = std::min(config_.nprobe, config_.nlist);
      auto probe_clusters = FindNearestCentroids(query_vec, nprobe);

      // Bounded min-heap for top-k selection
      using ScoreIdx = std::pair<float, size_t>;
      auto cmp = [](const ScoreIdx& a, const ScoreIdx& b) { return a.first > b.first; };
      std::priority_queue<ScoreIdx, std::vector<ScoreIdx>, decltype(cmp)> min_heap(cmp);

      // Scan vectors in the selected clusters
      for (size_t cluster_id : probe_clusters) {
        const auto& list = inverted_lists_[cluster_id];

        for (size_t li = 0; li < list.size(); ++li) {
          // Prefetch upcoming vector data
          if (li + kPrefetchAhead < list.size()) {
            size_t prefetch_idx = list[li + kPrefetchAhead];
            if (prefetch_idx < total_count) {
              __builtin_prefetch(matrix + prefetch_idx * dimension, 0, 0);
            }
          }

          size_t idx = list[li];
          if (idx >= total_count) {
            continue;  // Stale index after defragmentation
          }

          float cand_norm = norms[idx];
          float score = ScoreCandidate(query_vec, query_norm, matrix + idx * dimension, cand_norm, dimension);

          if (min_heap.size() < top_k) {
            min_heap.push({score, idx});
          } else if (score > min_heap.top().first) {
            min_heap.pop();
            min_heap.push({score, idx});
          }
        }
      }

      // Extract IVF results
      ivf_results.reserve(min_heap.size());
      while (!min_heap.empty()) {
        ivf_results.push_back(min_heap.top());
        min_heap.pop();
      }
    }
  }

  // If only one source has results, skip merge
  if (buffer_results.empty()) {
    std::sort(ivf_results.begin(), ivf_results.end(),
              [](const std::pair<float, size_t>& a, const std::pair<float, size_t>& b) { return a.first > b.first; });
    return ivf_results;
  }
  if (ivf_results.empty()) {
    // buffer_results is already sorted descending from SearchBuffer
    return buffer_results;
  }

  // Merge and deduplicate: keep highest score per compact_index
  std::unordered_set<size_t> seen;
  seen.reserve(ivf_results.size() + buffer_results.size());

  using ScoreIdx = std::pair<float, size_t>;
  auto cmp = [](const ScoreIdx& a, const ScoreIdx& b) { return a.first > b.first; };
  std::priority_queue<ScoreIdx, std::vector<ScoreIdx>, decltype(cmp)> merge_heap(cmp);

  // Buffer results are typically from more recent data; add them first
  for (const auto& [score, idx] : buffer_results) {
    if (seen.insert(idx).second) {
      if (merge_heap.size() < top_k) {
        merge_heap.push({score, idx});
      } else if (score > merge_heap.top().first) {
        merge_heap.pop();
        merge_heap.push({score, idx});
      }
    }
  }

  for (const auto& [score, idx] : ivf_results) {
    if (seen.insert(idx).second) {
      if (merge_heap.size() < top_k) {
        merge_heap.push({score, idx});
      } else if (score > merge_heap.top().first) {
        merge_heap.pop();
        merge_heap.push({score, idx});
      }
    }
  }

  // Extract merged results sorted by score descending
  std::vector<std::pair<float, size_t>> results;
  results.reserve(merge_heap.size());
  while (!merge_heap.empty()) {
    results.push_back(merge_heap.top());
    merge_heap.pop();
  }
  std::sort(results.begin(), results.end(), [](const ScoreIdx& a, const ScoreIdx& b) { return a.first > b.first; });

  return results;
}

bool IvfIndex::IsTrained() const {
  std::shared_lock lock(mutex_);
  return trained_;
}

void IvfIndex::ResetTrained() {
  std::unique_lock lock(mutex_);
  // Restore the operator-configured nlist. Train derives the effective value
  // from it, so the auto sentinel still recalculates for the new data size
  // while an explicit setting survives the reset.
  config_.nlist = configured_nlist_;
  trained_ = false;
  ++layout_generation_;
}

void IvfIndex::Reset(uint32_t dimension) {
  std::unique_lock lock(mutex_);
  dimension_.store(dimension, std::memory_order_relaxed);

  config_.nlist = configured_nlist_;
  trained_ = false;
  centroids_.clear();
  centroid_norms_.clear();
  inverted_lists_.clear();
  cluster_of_.clear();
  ++layout_generation_;

  std::unique_lock buf_lock(buffer_mutex_);
  ClearTier(buffer_indices_, buffer_vectors_, buffer_norms_, buffer_slots_);
  ClearTier(sealing_indices_, sealing_vectors_, sealing_norms_, sealing_slots_);
}

uint32_t IvfIndex::GetDimension() const {
  return dimension_.load(std::memory_order_relaxed);
}

size_t IvfIndex::GetIndexedCount() const {
  std::shared_lock lock(mutex_);
  size_t count = 0;
  for (const auto& list : inverted_lists_) {
    count += list.size();
  }
  return count;
}

size_t IvfIndex::GetClusterCount() const {
  std::shared_lock lock(mutex_);
  return trained_ ? config_.nlist : 0;
}

void IvfIndex::SetNprobe(uint32_t nprobe) {
  std::unique_lock lock(mutex_);
  config_.nprobe = std::min(nprobe, config_.nlist);
}

uint32_t IvfIndex::GetNprobe() const {
  std::shared_lock lock(mutex_);
  return config_.nprobe;
}

void IvfIndex::SetNlist(uint32_t nlist) {
  std::unique_lock lock(mutex_);
  configured_nlist_ = nlist;
  config_.nlist = nlist;
  ++layout_generation_;
}

// ============================================================================
// Write Buffer Operations
// ============================================================================

void IvfIndex::AppendToBuffer(size_t compact_index, const float* vector) {
  if (vector == nullptr) {
    return;
  }

  const uint32_t dim = dimension_.load(std::memory_order_relaxed);
  std::unique_lock lock(buffer_mutex_);

  // Upsert: if this compact_index is already pending in the buffer (a re-VECSET
  // of the same id before the buffer was sealed), overwrite its vector in place
  // instead of appending a duplicate. Two live entries for one id would return
  // the item twice from Search and keep ranking it by the stale embedding.
  const float norm = config_.metric == IvfMetric::kCosine ? simd::GetOptimalImpl().l2_norm(vector, dim) : 0.0F;
  auto existing = buffer_slots_.find(compact_index);
  if (existing != buffer_slots_.end()) {
    std::copy(vector, vector + dim, buffer_vectors_.begin() + static_cast<ptrdiff_t>(existing->second * dim));
    buffer_norms_[existing->second] = norm;
    return;
  }

  PushToTier(compact_index, vector, norm, dim, buffer_indices_, buffer_vectors_, buffer_norms_, buffer_slots_);
}

void IvfIndex::RestoreSealingToBufferLocked() {
  const uint32_t dim = dimension_.load(std::memory_order_relaxed);
  for (size_t i = 0; i < sealing_indices_.size(); ++i) {
    if (buffer_slots_.count(sealing_indices_[i]) != 0) {
      continue;  // A newer buffered embedding wins.
    }
    PushToTier(sealing_indices_[i], sealing_vectors_.data() + i * dim, sealing_norms_[i], dim, buffer_indices_,
               buffer_vectors_, buffer_norms_, buffer_slots_);
  }
  ClearTier(sealing_indices_, sealing_vectors_, sealing_norms_, sealing_slots_);
}

size_t IvfIndex::GetBufferSize() const {
  std::shared_lock lock(buffer_mutex_);
  return buffer_indices_.size() + sealing_indices_.size();
}

size_t IvfIndex::GetSealingSize() const {
  std::shared_lock lock(buffer_mutex_);
  return sealing_indices_.size();
}

bool IvfIndex::NeedsSeal() const {
  std::shared_lock lock(buffer_mutex_);
  return buffer_indices_.size() >= config_.seal_threshold;
}

bool IvfIndex::SealBuffer() {
  // Phase 1: Move buffer contents into a distinct searchable sealing tier.
  std::vector<size_t> seal_indices;
  std::vector<float> seal_vectors;
  std::vector<float> seal_norms;
  {
    std::unique_lock buf_lock(buffer_mutex_);
    if (!sealing_indices_.empty()) {
      return false;  // Another seal owns the pending entries.
    }
    if (buffer_indices_.empty()) {
      return true;  // Nothing pending.
    }
    sealing_indices_.swap(buffer_indices_);
    sealing_vectors_.swap(buffer_vectors_);
    sealing_norms_.swap(buffer_norms_);
    sealing_slots_.swap(buffer_slots_);
    seal_indices = sealing_indices_;
    seal_vectors = sealing_vectors_;
    seal_norms = sealing_norms_;
  }
  // Buffer lock released: AppendToBuffer can proceed immediately

  // Phase 2: Snapshot the immutable IVF layout, then do the O(n * k * d)
  // assignments without holding mutex_. Searchers retain their shared lock
  // throughout this work instead of being blocked by a seal.
  const uint32_t dim = dimension_.load(std::memory_order_relaxed);
  uint32_t nlist = 0;
  IvfMetric metric = IvfMetric::kCosine;
  uint64_t layout_generation = 0;
  std::vector<float> centroids;
  std::vector<float> centroid_norms;
  {
    std::shared_lock ivf_lock(mutex_);
    if (!trained_) {
      std::unique_lock buf_lock(buffer_mutex_);
      RestoreSealingToBufferLocked();
      return false;
    }
    nlist = config_.nlist;
    metric = config_.metric;
    layout_generation = layout_generation_;
    centroids = centroids_;
    centroid_norms = centroid_norms_;
  }

  std::vector<size_t> cluster_assignments(seal_indices.size());
  for (size_t i = 0; i < seal_indices.size(); ++i) {
    const float* vec = seal_vectors.data() + i * dim;
    // Tier norms are only maintained for cosine; other metrics ignore the norm.
    const float vec_norm = metric == IvfMetric::kCosine ? seal_norms[i] : 0.0F;
    cluster_assignments[i] =
        AssignCluster(metric, centroids.data(), centroid_norms.data(), nlist, vec, vec_norm, seal_indices[i], dim);
  }

  // Phase 3: Commit only if the layout we assigned against is still current.
  // If a concurrent Train/Reset changed it, preserve the entries in the write
  // buffer so the next seal assigns them against the new centroids.
  {
    std::unique_lock ivf_lock(mutex_);
    if (!trained_ || layout_generation_ != layout_generation) {
      std::unique_lock buf_lock(buffer_mutex_);
      RestoreSealingToBufferLocked();
      return false;
    }
    std::unique_lock buf_lock(buffer_mutex_);
    for (size_t i = 0; i < seal_indices.size(); ++i) {
      // Skip anything a concurrent RemoveVector took out of the sealing tier
      // while the assignment ran off-lock, and anything overwritten since: its
      // newer embedding is in the write buffer and is assigned by the next seal.
      if (sealing_slots_.count(seal_indices[i]) != 0 && buffer_slots_.count(seal_indices[i]) == 0) {
        AppendToListLocked(cluster_assignments[i], seal_indices[i]);
        // A training install rebuilds the lists from its own snapshot, so a row
        // sealed meanwhile must be replayed or it leaves both tiers.
        if (recording_changes_) {
          const float* vec = seal_vectors.data() + i * dim;
          change_log_.push_back({seal_indices[i], false, std::vector<float>(vec, vec + dim)});
        }
      }
    }
    ClearTier(sealing_indices_, sealing_vectors_, sealing_norms_, sealing_slots_);
  }

  utils::StructuredLog()
      .Event("ivf_buffer_sealed")
      .Field("sealed_count", static_cast<int64_t>(seal_indices.size()))
      .Info();
  return true;
}

std::vector<std::pair<float, size_t>> IvfIndex::SearchBuffer(const float* query_vec, float query_norm,
                                                             uint32_t dimension, size_t top_k) const {
  if (top_k == 0 || dimension != dimension_.load(std::memory_order_relaxed)) {
    return {};
  }
  std::shared_lock lock(buffer_mutex_);

  if ((buffer_indices_.empty() && sealing_indices_.empty()) || query_vec == nullptr) {
    return {};
  }

  // Score both transient tiers and deduplicate by compact index. A newer
  // buffered embedding may coexist with the older sealing copy.
  using ScoreIdx = std::pair<float, size_t>;
  std::unordered_map<size_t, float> best_scores;
  best_scores.reserve(buffer_indices_.size() + sealing_indices_.size());
  const auto score_tier = [&](const std::vector<size_t>& indices, const std::vector<float>& vectors,
                              const std::vector<float>& norms, bool overwrite_existing) {
    for (size_t i = 0; i < indices.size(); ++i) {
      const float* vec = vectors.data() + i * dimension;
      const float vec_norm = norms[i];
      const float score = ScoreCandidate(query_vec, query_norm, vec, vec_norm, dimension);
      auto [it, inserted] = best_scores.emplace(indices[i], score);
      if (!inserted && (overwrite_existing || score > it->second)) {
        it->second = score;
      }
    }
  };
  score_tier(sealing_indices_, sealing_vectors_, sealing_norms_, false);
  score_tier(buffer_indices_, buffer_vectors_, buffer_norms_, true);  // Newer writes win.

  // Bounded min-heap for top-k selection.
  auto cmp = [](const ScoreIdx& a, const ScoreIdx& b) { return a.first > b.first; };
  std::priority_queue<ScoreIdx, std::vector<ScoreIdx>, decltype(cmp)> min_heap(cmp);
  for (const auto& [index, score] : best_scores) {
    if (min_heap.size() < top_k) {
      min_heap.push({score, index});
    } else if (score > min_heap.top().first) {
      min_heap.pop();
      min_heap.push({score, index});
    }
  }

  // Extract results sorted by score descending
  std::vector<std::pair<float, size_t>> results;
  results.reserve(min_heap.size());
  while (!min_heap.empty()) {
    results.push_back(min_heap.top());
    min_heap.pop();
  }
  std::sort(results.begin(), results.end(), [](const ScoreIdx& a, const ScoreIdx& b) { return a.first > b.first; });

  return results;
}

// ============================================================================
// K-Means Training (Lloyd's Algorithm)
// ============================================================================

void IvfIndex::KMeansTrain(const float* matrix, const size_t* sample_indices, size_t sample_size, uint32_t dim,
                           uint32_t nlist, IvfMetric metric, uint32_t max_iterations, float convergence_threshold,
                           std::vector<float>& centroids, std::vector<float>& centroid_norms) {
  // Random initialization: pick nlist random vectors as initial centroids.
  // Seeding consumes one distinct sample per cluster, so nlist <= sample_size.
  std::vector<size_t> init_indices(sample_size);
  std::iota(init_indices.begin(), init_indices.end(), size_t{0});

  thread_local std::mt19937 rng(std::random_device{}());
  std::shuffle(init_indices.begin(), init_indices.end(), rng);
  std::uniform_int_distribution<size_t> sample_distribution(0, sample_size - 1);

  centroids.resize(static_cast<size_t>(nlist) * dim);
  for (uint32_t c = 0; c < nlist; ++c) {
    size_t vec_idx = sample_indices[init_indices[c]];
    const float* src = matrix + vec_idx * dim;
    std::copy(src, src + dim, centroids.data() + static_cast<size_t>(c) * dim);
  }

  // Lloyd's iterations
  std::vector<size_t> assignments(sample_size);
  std::vector<float> new_centroids(static_cast<size_t>(nlist) * dim);
  std::vector<size_t> cluster_sizes(nlist);

  const auto& simd_impl = simd::GetOptimalImpl();

  const auto refresh_norms = [&] {
    centroid_norms.resize(nlist);
    for (uint32_t c = 0; c < nlist; ++c) {
      centroid_norms[c] = simd_impl.l2_norm(centroids.data() + static_cast<size_t>(c) * dim, dim);
    }
  };
  refresh_norms();

  const auto nearest_centroid = [&](const float* vec) {
    const float vec_norm = simd_impl.l2_norm(vec, dim);
    size_t best = 0;
    float best_sim = kWorstScore;
    for (uint32_t c = 0; c < nlist; ++c) {
      const float sim =
          SimilarityFor(metric, vec, vec_norm, centroids.data() + static_cast<size_t>(c) * dim, centroid_norms[c], dim);
      if (sim > best_sim) {
        best_sim = sim;
        best = c;
      }
    }
    return best;
  };

  for (uint32_t iter = 0; iter < max_iterations; ++iter) {
    // Assignment step: assign each sample to nearest centroid
    for (size_t i = 0; i < sample_size; ++i) {
      assignments[i] = nearest_centroid(matrix + sample_indices[i] * dim);
    }

    // Update step: recompute centroids
    std::fill(new_centroids.begin(), new_centroids.end(), 0.0F);
    std::fill(cluster_sizes.begin(), cluster_sizes.end(), size_t{0});

    for (size_t i = 0; i < sample_size; ++i) {
      size_t cluster = assignments[i];
      const float* vec = matrix + sample_indices[i] * dim;
      float* centroid = new_centroids.data() + cluster * dim;
      for (uint32_t d = 0; d < dim; ++d) {
        centroid[d] += vec[d];
      }
      ++cluster_sizes[cluster];
    }

    // Divide by cluster size to get mean; handle empty clusters
    for (uint32_t c = 0; c < nlist; ++c) {
      float* centroid = new_centroids.data() + static_cast<size_t>(c) * dim;
      if (cluster_sizes[c] > 0) {
        float inv_size = 1.0F / static_cast<float>(cluster_sizes[c]);
        for (uint32_t d = 0; d < dim; ++d) {
          centroid[d] *= inv_size;
        }
      } else {
        // Empty cluster: reinitialize with a random sample
        size_t random_sample = sample_indices[sample_distribution(rng)];
        const float* src = matrix + random_sample * dim;
        std::copy(src, src + dim, centroid);
      }
    }

    // Check convergence: maximum centroid movement
    float max_delta = 0.0F;
    for (uint32_t c = 0; c < nlist; ++c) {
      float delta = simd_impl.l2_distance(centroids.data() + static_cast<size_t>(c) * dim,
                                          new_centroids.data() + static_cast<size_t>(c) * dim, dim);
      max_delta = std::max(max_delta, delta);
    }

    centroids = new_centroids;
    refresh_norms();

    if (max_delta < convergence_threshold) {
      utils::StructuredLog()
          .Event("ivf_kmeans_converged")
          .Field("iterations", static_cast<int64_t>(iter + 1))
          .Field("max_delta", static_cast<double>(max_delta))
          .Debug();
      break;
    }
  }
}

size_t IvfIndex::FindNearestCentroid(const float* vec) const {
  const auto& simd_impl = simd::GetOptimalImpl();
  const uint32_t dim = dimension_.load(std::memory_order_relaxed);
  float vec_norm = simd_impl.l2_norm(vec, dim);

  size_t best = 0;
  float best_sim = kWorstScore;

  for (uint32_t c = 0; c < config_.nlist; ++c) {
    float sim = Similarity(vec, vec_norm, centroids_.data() + static_cast<size_t>(c) * dim, centroid_norms_[c], dim);
    if (sim > best_sim) {
      best_sim = sim;
      best = c;
    }
  }

  // Zero-norm vectors under cosine score -inf everywhere and fall back to
  // cluster 0. This single-vector path (AddVector / k-means assignment) has no
  // index to spread by; bulk paths handle the spreading explicitly.
  return best;
}

std::vector<size_t> IvfIndex::FindNearestCentroids(const float* vec, uint32_t nprobe) const {
  const auto& simd_impl = simd::GetOptimalImpl();
  const uint32_t dim = dimension_.load(std::memory_order_relaxed);
  float vec_norm = simd_impl.l2_norm(vec, dim);

  // Compute similarity to all centroids (higher == nearer for every metric)
  std::vector<std::pair<float, size_t>> centroid_sims(config_.nlist);
  for (uint32_t c = 0; c < config_.nlist; ++c) {
    float sim = Similarity(vec, vec_norm, centroids_.data() + static_cast<size_t>(c) * dim, centroid_norms_[c], dim);
    centroid_sims[c] = {sim, c};
  }

  // Partial sort to find nprobe nearest
  uint32_t actual_nprobe = std::min(nprobe, config_.nlist);
  std::partial_sort(centroid_sims.begin(), centroid_sims.begin() + actual_nprobe, centroid_sims.end(),
                    [](const std::pair<float, size_t>& a, const std::pair<float, size_t>& b) {
                      return a.first > b.first;  // Higher similarity first
                    });

  std::vector<size_t> result(actual_nprobe);
  for (uint32_t i = 0; i < actual_nprobe; ++i) {
    result[i] = centroid_sims[i].second;
  }

  return result;
}

}  // namespace nvecd::vectors
