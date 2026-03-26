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
#include <numeric>
#include <queue>
#include <random>
#include <unordered_set>

#include "utils/structured_log.h"
#include "vectors/distance.h"
#include "vectors/distance_simd.h"

namespace nvecd::vectors {

namespace {

/// Maximum number of training samples for K-means
constexpr size_t kMaxTrainingSamples = 10000;

/// Prefetch ahead distance for search loop
constexpr size_t kPrefetchAhead = 4;

/// Minimum norm threshold to avoid division by zero
constexpr float kNormEpsilon = 1e-7F;

}  // namespace

IvfIndex::IvfIndex(uint32_t dimension)
    : config_(), dimension_(dimension) {}

IvfIndex::IvfIndex(uint32_t dimension, const Config& config)
    : config_(config), dimension_(dimension) {}

void IvfIndex::Train(const float* matrix, const size_t* valid_indices,
                     size_t num_valid, uint32_t dimension, bool assign_vectors) {
  // Prevent concurrent training attempts
  bool expected = false;
  if (!training_in_progress_.compare_exchange_strong(expected, true)) {
    return;  // Another thread is already training
  }

  // Ensure training_in_progress_ is reset when we exit
  struct TrainingGuard {
    std::atomic<bool>& flag;
    ~TrainingGuard() { flag.store(false); }
  } guard{training_in_progress_};

  std::unique_lock lock(mutex_);

  if (num_valid == 0 || matrix == nullptr || valid_indices == nullptr) {
    return;
  }

  dimension_ = dimension;

  // Auto-scale nlist if set to 0: use sqrt(n), capped at kMaxAutoNlist
  if (config_.nlist == 0) {
    auto sqrt_n = static_cast<uint32_t>(
        std::max(1.0, std::sqrt(static_cast<double>(num_valid))));
    config_.nlist = std::min(sqrt_n, kMaxAutoNlist);
  }

  // Clamp nlist to not exceed the number of vectors
  if (config_.nlist > num_valid) {
    config_.nlist = static_cast<uint32_t>(num_valid);
  }

  // Subsample if too many vectors for training
  std::vector<size_t> train_indices;
  const size_t* train_ptr = valid_indices;
  size_t train_size = num_valid;

  if (num_valid > kMaxTrainingSamples) {
    // Random sample using reservoir sampling
    train_indices.resize(kMaxTrainingSamples);
    std::copy(valid_indices, valid_indices + kMaxTrainingSamples,
              train_indices.begin());

    thread_local std::mt19937 rng(std::random_device{}());
    for (size_t i = kMaxTrainingSamples; i < num_valid; ++i) {
      size_t j = rng() % (i + 1);
      if (j < kMaxTrainingSamples) {
        train_indices[j] = valid_indices[i];
      }
    }

    train_ptr = train_indices.data();
    train_size = kMaxTrainingSamples;
  }

  // Run K-means
  KMeansTrain(matrix, train_ptr, train_size, dimension);

  // Initialize inverted lists
  inverted_lists_.clear();
  inverted_lists_.resize(config_.nlist);

  // Assign vectors to clusters if requested
  if (assign_vectors) {
    const auto& simd_impl = simd::GetOptimalImpl();
    const uint32_t nlist = config_.nlist;

    for (size_t i = 0; i < num_valid; ++i) {
      size_t idx = valid_indices[i];
      const float* vec = matrix + idx * dimension;
      float vec_norm = simd_impl.l2_norm(vec, dimension);

      size_t best = 0;
      float best_sim = -2.0F;

      for (uint32_t c = 0; c < nlist; ++c) {
        float cn = centroid_norms_[c];
        if (cn < kNormEpsilon || vec_norm < kNormEpsilon) {
          continue;
        }
        float dot = simd_impl.dot_product(
            vec, centroids_.data() + static_cast<size_t>(c) * dimension,
            dimension);
        float sim = dot / (vec_norm * cn);
        if (sim > best_sim) {
          best_sim = sim;
          best = c;
        }
      }

      inverted_lists_[best].push_back(idx);
    }
  }

  trained_ = true;

  // Log training completion
  size_t total_indexed = 0;
  for (const auto& list : inverted_lists_) {
    total_indexed += list.size();
  }
  utils::StructuredLog()
      .Event("ivf_index_trained")
      .Field("nlist", static_cast<int64_t>(config_.nlist))
      .Field("nprobe", static_cast<int64_t>(config_.nprobe))
      .Field("indexed_count", static_cast<int64_t>(total_indexed))
      .Field("dimension", static_cast<int64_t>(dimension))
      .Info();
}

void IvfIndex::AddVector(size_t compact_index, const float* vector) {
  std::unique_lock lock(mutex_);

  if (!trained_ || vector == nullptr) {
    return;
  }

  size_t cluster = FindNearestCentroid(vector);
  inverted_lists_[cluster].push_back(compact_index);
}

void IvfIndex::BulkAddVectors(const size_t* compact_indices, const float* vectors,
                               size_t count, uint32_t dimension) {
  std::unique_lock lock(mutex_);

  if (!trained_ || compact_indices == nullptr || vectors == nullptr || count == 0) {
    return;
  }

  const auto& simd_impl = simd::GetOptimalImpl();
  const uint32_t nlist = config_.nlist;

  for (size_t i = 0; i < count; ++i) {
    size_t idx = compact_indices[i];
    const float* vec = vectors + i * dimension;  // Row i of the contiguous array
    float vec_norm = simd_impl.l2_norm(vec, dimension);

    size_t best = 0;
    float best_sim = -2.0F;

    for (uint32_t c = 0; c < nlist; ++c) {
      float cn = centroid_norms_[c];
      if (cn < kNormEpsilon || vec_norm < kNormEpsilon) {
        continue;
      }
      float dot = simd_impl.dot_product(
          vec, centroids_.data() + static_cast<size_t>(c) * dimension,
          dimension);
      float sim = dot / (vec_norm * cn);
      if (sim > best_sim) {
        best_sim = sim;
        best = c;
      }
    }

    inverted_lists_[best].push_back(idx);
  }
}

void IvfIndex::RemoveVector(size_t compact_index) {
  // First check the write buffer
  {
    std::unique_lock buf_lock(buffer_mutex_);
    for (size_t i = 0; i < buffer_indices_.size(); ++i) {
      if (buffer_indices_[i] == compact_index) {
        // Swap with last and pop for O(1) removal
        size_t last = buffer_indices_.size() - 1;
        if (i != last) {
          buffer_indices_[i] = buffer_indices_[last];
          // Swap vector data
          std::copy(
              buffer_vectors_.data() + last * dimension_,
              buffer_vectors_.data() + (last + 1) * dimension_,
              buffer_vectors_.data() + i * dimension_);
        }
        buffer_indices_.pop_back();
        buffer_vectors_.resize(buffer_indices_.size() * dimension_);
        return;
      }
    }
  }

  // Then check IVF inverted lists
  std::unique_lock lock(mutex_);

  if (!trained_) {
    return;
  }

  // Search all inverted lists for the compact_index
  for (auto& list : inverted_lists_) {
    auto it = std::find(list.begin(), list.end(), compact_index);
    if (it != list.end()) {
      // Swap with last and pop for O(1) removal
      *it = list.back();
      list.pop_back();
      return;
    }
  }
}

std::vector<std::pair<float, size_t>> IvfIndex::Search(
    const float* query_vec, float query_norm,
    const float* matrix, const float* norms,
    size_t total_count, uint32_t dimension,
    size_t top_k) const {
  // Search the write buffer first (independent lock)
  auto buffer_results = SearchBuffer(query_vec, query_norm, dimension, top_k);

  // Search the IVF inverted lists
  std::vector<std::pair<float, size_t>> ivf_results;
  {
    std::shared_lock lock(mutex_);

    if (trained_ && query_vec != nullptr && matrix != nullptr &&
        norms != nullptr && total_count > 0) {
      // Find nprobe nearest centroids
      uint32_t nprobe = std::min(config_.nprobe, config_.nlist);
      auto probe_clusters = FindNearestCentroids(query_vec, nprobe);

      // Bounded min-heap for top-k selection
      using ScoreIdx = std::pair<float, size_t>;
      auto cmp = [](const ScoreIdx& a, const ScoreIdx& b) {
        return a.first > b.first;
      };
      std::priority_queue<ScoreIdx, std::vector<ScoreIdx>, decltype(cmp)>
          min_heap(cmp);

      const auto& simd_impl = simd::GetOptimalImpl();

      // Scan vectors in the selected clusters
      for (size_t cluster_id : probe_clusters) {
        const auto& list = inverted_lists_[cluster_id];

        for (size_t li = 0; li < list.size(); ++li) {
          // Prefetch upcoming vector data
          if (li + kPrefetchAhead < list.size()) {
            size_t prefetch_idx = list[li + kPrefetchAhead];
            if (prefetch_idx < total_count) {
              __builtin_prefetch(
                  matrix + prefetch_idx * dimension, 0, 0);
            }
          }

          size_t idx = list[li];
          if (idx >= total_count) {
            continue;  // Stale index after defragmentation
          }

          float cand_norm = norms[idx];
          if (cand_norm < kNormEpsilon || query_norm < kNormEpsilon) {
            continue;
          }

          float dot = simd_impl.dot_product(
              query_vec, matrix + idx * dimension, dimension);
          float score = dot / (query_norm * cand_norm);

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
              [](const std::pair<float, size_t>& a,
                 const std::pair<float, size_t>& b) {
                return a.first > b.first;
              });
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
  auto cmp = [](const ScoreIdx& a, const ScoreIdx& b) {
    return a.first > b.first;
  };
  std::priority_queue<ScoreIdx, std::vector<ScoreIdx>, decltype(cmp)>
      merge_heap(cmp);

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
  std::sort(results.begin(), results.end(),
            [](const ScoreIdx& a, const ScoreIdx& b) {
              return a.first > b.first;
            });

  return results;
}

bool IvfIndex::IsTrained() const {
  std::shared_lock lock(mutex_);
  return trained_;
}

void IvfIndex::ResetTrained() {
  // Reset nlist to 0 so auto-scaling recalculates for the new data size
  config_.nlist = 0;
  trained_ = false;
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
  config_.nlist = nlist;
}

// ============================================================================
// Write Buffer Operations
// ============================================================================

void IvfIndex::AppendToBuffer(size_t compact_index, const float* vector) {
  if (vector == nullptr) {
    return;
  }

  std::unique_lock lock(buffer_mutex_);
  buffer_indices_.push_back(compact_index);
  buffer_vectors_.insert(buffer_vectors_.end(), vector, vector + dimension_);
}

size_t IvfIndex::GetBufferSize() const {
  std::shared_lock lock(buffer_mutex_);
  return buffer_indices_.size();
}

bool IvfIndex::NeedsSeal() const {
  std::shared_lock lock(buffer_mutex_);
  return buffer_indices_.size() >= config_.seal_threshold;
}

void IvfIndex::SealBuffer() {
  // Phase 1: Swap buffer contents out under brief lock (fast, unblocks writers)
  std::vector<size_t> seal_indices;
  std::vector<float> seal_vectors;
  {
    std::unique_lock buf_lock(buffer_mutex_);
    if (buffer_indices_.empty()) {
      return;
    }
    seal_indices.swap(buffer_indices_);
    seal_vectors.swap(buffer_vectors_);
  }
  // Buffer lock released: AppendToBuffer can proceed immediately

  // Phase 2: Assign extracted entries to IVF clusters (expensive, lock-free on buffer)
  std::unique_lock ivf_lock(mutex_);

  if (!trained_) {
    // IVF not trained: put entries back in buffer
    std::unique_lock buf_lock(buffer_mutex_);
    buffer_indices_.insert(buffer_indices_.end(),
                           seal_indices.begin(), seal_indices.end());
    buffer_vectors_.insert(buffer_vectors_.end(),
                           seal_vectors.begin(), seal_vectors.end());
    return;
  }

  const auto& simd_impl = simd::GetOptimalImpl();
  const uint32_t nlist = config_.nlist;
  size_t sealed_count = seal_indices.size();

  for (size_t i = 0; i < sealed_count; ++i) {
    const float* vec = seal_vectors.data() + i * dimension_;
    float vec_norm = simd_impl.l2_norm(vec, dimension_);

    size_t best = 0;
    float best_sim = -2.0F;

    for (uint32_t c = 0; c < nlist; ++c) {
      float cn = centroid_norms_[c];
      if (cn < kNormEpsilon || vec_norm < kNormEpsilon) {
        continue;
      }
      float dot = simd_impl.dot_product(
          vec, centroids_.data() + static_cast<size_t>(c) * dimension_,
          dimension_);
      float sim = dot / (vec_norm * cn);
      if (sim > best_sim) {
        best_sim = sim;
        best = c;
      }
    }

    inverted_lists_[best].push_back(seal_indices[i]);
  }

  utils::StructuredLog()
      .Event("ivf_buffer_sealed")
      .Field("sealed_count", static_cast<int64_t>(sealed_count))
      .Info();
}

std::vector<std::pair<float, size_t>> IvfIndex::SearchBuffer(
    const float* query_vec, float query_norm,
    uint32_t dimension, size_t top_k) const {
  std::shared_lock lock(buffer_mutex_);

  if (buffer_indices_.empty() || query_vec == nullptr ||
      query_norm < kNormEpsilon) {
    return {};
  }

  const auto& simd_impl = simd::GetOptimalImpl();

  // Bounded min-heap for top-k selection
  using ScoreIdx = std::pair<float, size_t>;
  auto cmp = [](const ScoreIdx& a, const ScoreIdx& b) {
    return a.first > b.first;
  };
  std::priority_queue<ScoreIdx, std::vector<ScoreIdx>, decltype(cmp)>
      min_heap(cmp);

  for (size_t i = 0; i < buffer_indices_.size(); ++i) {
    // Prefetch upcoming buffer vector data
    if (i + kPrefetchAhead < buffer_indices_.size()) {
      __builtin_prefetch(
          buffer_vectors_.data() + (i + kPrefetchAhead) * dimension, 0, 0);
    }

    const float* vec = buffer_vectors_.data() + i * dimension;
    float vec_norm = simd_impl.l2_norm(vec, dimension);

    if (vec_norm < kNormEpsilon) {
      continue;
    }

    float dot = simd_impl.dot_product(query_vec, vec, dimension);
    float score = dot / (query_norm * vec_norm);

    if (min_heap.size() < top_k) {
      min_heap.push({score, buffer_indices_[i]});
    } else if (score > min_heap.top().first) {
      min_heap.pop();
      min_heap.push({score, buffer_indices_[i]});
    }
  }

  // Extract results sorted by score descending
  std::vector<std::pair<float, size_t>> results;
  results.reserve(min_heap.size());
  while (!min_heap.empty()) {
    results.push_back(min_heap.top());
    min_heap.pop();
  }
  std::sort(results.begin(), results.end(),
            [](const ScoreIdx& a, const ScoreIdx& b) {
              return a.first > b.first;
            });

  return results;
}

// ============================================================================
// K-Means Training (Lloyd's Algorithm)
// ============================================================================

void IvfIndex::KMeansTrain(const float* matrix, const size_t* sample_indices,
                           size_t sample_size, uint32_t dim) {
  const uint32_t nlist = config_.nlist;

  // Random initialization: pick nlist random vectors as initial centroids
  std::vector<size_t> init_indices(sample_size);
  std::iota(init_indices.begin(), init_indices.end(), size_t{0});

  thread_local std::mt19937 rng(std::random_device{}());
  std::shuffle(init_indices.begin(), init_indices.end(), rng);

  centroids_.resize(static_cast<size_t>(nlist) * dim);
  for (uint32_t c = 0; c < nlist; ++c) {
    size_t vec_idx = sample_indices[init_indices[c]];
    const float* src = matrix + vec_idx * dim;
    std::copy(src, src + dim, centroids_.data() + static_cast<size_t>(c) * dim);
  }

  // Lloyd's iterations
  std::vector<size_t> assignments(sample_size);
  std::vector<float> new_centroids(static_cast<size_t>(nlist) * dim);
  std::vector<size_t> cluster_sizes(nlist);

  const auto& simd_impl = simd::GetOptimalImpl();

  // Compute initial centroid norms before first iteration
  centroid_norms_.resize(nlist);
  for (uint32_t c = 0; c < nlist; ++c) {
    centroid_norms_[c] = simd_impl.l2_norm(
        centroids_.data() + static_cast<size_t>(c) * dim, dim);
  }

  for (uint32_t iter = 0; iter < config_.max_iterations; ++iter) {
    // Assignment step: assign each sample to nearest centroid
    for (size_t i = 0; i < sample_size; ++i) {
      size_t vec_idx = sample_indices[i];
      const float* vec = matrix + vec_idx * dim;
      assignments[i] = FindNearestCentroid(vec);
    }

    // Update step: recompute centroids
    std::fill(new_centroids.begin(), new_centroids.end(), 0.0F);
    std::fill(cluster_sizes.begin(), cluster_sizes.end(), size_t{0});

    for (size_t i = 0; i < sample_size; ++i) {
      size_t cluster = assignments[i];
      size_t vec_idx = sample_indices[i];
      const float* vec = matrix + vec_idx * dim;
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
        size_t random_sample = sample_indices[rng() % sample_size];
        const float* src = matrix + random_sample * dim;
        std::copy(src, src + dim, centroid);
      }
    }

    // Check convergence: maximum centroid movement
    float max_delta = 0.0F;
    for (uint32_t c = 0; c < nlist; ++c) {
      float delta = simd_impl.l2_distance(
          centroids_.data() + static_cast<size_t>(c) * dim,
          new_centroids.data() + static_cast<size_t>(c) * dim,
          dim);
      max_delta = std::max(max_delta, delta);
    }

    centroids_ = new_centroids;

    // Recompute centroid norms for next iteration's FindNearestCentroid
    for (uint32_t c = 0; c < nlist; ++c) {
      centroid_norms_[c] = simd_impl.l2_norm(
          centroids_.data() + static_cast<size_t>(c) * dim, dim);
    }

    if (max_delta < config_.convergence_threshold) {
      utils::StructuredLog()
          .Event("ivf_kmeans_converged")
          .Field("iterations", static_cast<int64_t>(iter + 1))
          .Field("max_delta", static_cast<double>(max_delta))
          .Debug();
      break;
    }
  }

  // centroid_norms_ is already up to date from the last iteration
}

size_t IvfIndex::FindNearestCentroid(const float* vec) const {
  const auto& simd_impl = simd::GetOptimalImpl();
  float vec_norm = simd_impl.l2_norm(vec, dimension_);

  size_t best = 0;
  float best_sim = -2.0F;  // Cosine similarity range is [-1, 1]

  for (uint32_t c = 0; c < config_.nlist; ++c) {
    float cn = centroid_norms_[c];
    if (cn < kNormEpsilon || vec_norm < kNormEpsilon) {
      continue;
    }
    float dot = simd_impl.dot_product(
        vec, centroids_.data() + static_cast<size_t>(c) * dimension_,
        dimension_);
    float sim = dot / (vec_norm * cn);
    if (sim > best_sim) {
      best_sim = sim;
      best = c;
    }
  }

  return best;
}

std::vector<size_t> IvfIndex::FindNearestCentroids(const float* vec,
                                                    uint32_t nprobe) const {
  const auto& simd_impl = simd::GetOptimalImpl();
  float vec_norm = simd_impl.l2_norm(vec, dimension_);

  // Compute similarity to all centroids
  std::vector<std::pair<float, size_t>> centroid_sims(config_.nlist);
  for (uint32_t c = 0; c < config_.nlist; ++c) {
    float cn = centroid_norms_[c];
    float sim = 0.0F;
    if (cn >= kNormEpsilon && vec_norm >= kNormEpsilon) {
      float dot = simd_impl.dot_product(
          vec, centroids_.data() + static_cast<size_t>(c) * dimension_,
          dimension_);
      sim = dot / (vec_norm * cn);
    }
    centroid_sims[c] = {sim, c};
  }

  // Partial sort to find nprobe nearest
  uint32_t actual_nprobe = std::min(nprobe, config_.nlist);
  std::partial_sort(
      centroid_sims.begin(),
      centroid_sims.begin() + actual_nprobe,
      centroid_sims.end(),
      [](const std::pair<float, size_t>& a,
         const std::pair<float, size_t>& b) {
        return a.first > b.first;  // Higher similarity first
      });

  std::vector<size_t> result(actual_nprobe);
  for (uint32_t i = 0; i < actual_nprobe; ++i) {
    result[i] = centroid_sims[i].second;
  }

  return result;
}

}  // namespace nvecd::vectors
