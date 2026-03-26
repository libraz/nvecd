/**
 * @file ivf_index.h
 * @brief IVF (Inverted File) Approximate Nearest Neighbor index
 *
 * Provides an IVF index for accelerating vector similarity search.
 * Vectors are partitioned into Voronoi cells using K-means clustering,
 * and queries search only the nearest nprobe cells instead of all vectors.
 *
 * Uses a two-tier architecture (Milvus-style):
 * - Write buffer: flat, append-only buffer for recent vectors (brute-force searched)
 * - Sealed IVF index: trained clusters with inverted lists (IVF searched)
 * Search merges results from both tiers and returns top-k.
 *
 * Thread-safety:
 * - AppendToBuffer: exclusive lock on buffer_mutex_ only (fast path)
 * - SealBuffer: exclusive lock on both buffer_mutex_ and mutex_
 * - Train/AddVector/RemoveVector/BulkAddVectors: exclusive lock on mutex_
 * - Search/IsTrained/GetNprobe: shared lock on mutex_ + shared lock on buffer_mutex_
 */

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <shared_mutex>
#include <utility>
#include <vector>

namespace nvecd::vectors {

/**
 * @brief IVF (Inverted File) index for approximate nearest neighbor search
 *
 * Partitions the vector space into Voronoi cells using K-means clustering.
 * At query time, only the nearest nprobe cells are searched, reducing the
 * number of distance computations from O(n) to O(n * nprobe / nlist).
 *
 * Recent vectors are buffered in a flat write buffer and brute-force
 * searched. When the buffer reaches seal_threshold, it is sealed: buffer
 * entries are assigned to existing IVF clusters (or held until training).
 *
 * Example:
 * @code
 * IvfIndex::Config cfg;
 * cfg.nlist = 256;
 * cfg.nprobe = 8;
 * cfg.seal_threshold = 100000;
 * IvfIndex index(128, cfg);
 *
 * // Train on existing vectors
 * index.Train(matrix_ptr, valid_indices, num_valid, 128);
 *
 * // Append new vectors to write buffer (fast)
 * index.AppendToBuffer(compact_idx, vec_ptr);
 *
 * // Search merges IVF + buffer results automatically
 * auto results = index.Search(query, query_norm, matrix, norms,
 *                             total_count, 128, 10);
 *
 * // Seal buffer when ready
 * if (index.NeedsSeal()) {
 *   index.SealBuffer();
 * }
 * @endcode
 */
class IvfIndex {
 public:
  /// Maximum auto-nlist to cap training cost for large datasets
  static constexpr uint32_t kMaxAutoNlist = 1024;

  /**
   * @brief IVF index configuration
   */
  struct Config {
    uint32_t nlist = 256;                    ///< Number of Voronoi cells/clusters
    uint32_t nprobe = 8;                     ///< Clusters to search at query time
    uint32_t train_threshold = 10000;        ///< Min vectors before training
    uint32_t max_iterations = 10;            ///< K-means max iterations
    float convergence_threshold = 0.001F;    ///< K-means convergence threshold
    uint32_t seal_threshold = 100000;        ///< Seal write buffer at this size
  };

  /**
   * @brief Construct IVF index with default configuration
   * @param dimension Vector dimension
   */
  explicit IvfIndex(uint32_t dimension);

  /**
   * @brief Construct IVF index with custom configuration
   * @param dimension Vector dimension
   * @param config Index configuration
   */
  IvfIndex(uint32_t dimension, const Config& config);

  /**
   * @brief Train the index using K-means clustering
   *
   * Builds Voronoi cells from the provided vectors.
   *
   * @param matrix Pointer to contiguous [n x dim] float matrix
   * @param valid_indices Array of indices into the matrix that are not deleted
   * @param num_valid Number of valid indices
   * @param dimension Vector dimension
   * @param assign_vectors If true, assign all valid vectors to clusters after training.
   *        Set to false when caller will use AddVector() to assign separately.
   */
  void Train(const float* matrix, const size_t* valid_indices,
             size_t num_valid, uint32_t dimension, bool assign_vectors = true);

  /**
   * @brief Add a single vector directly to the IVF inverted lists
   *
   * Requires the index to be trained. Prefer AppendToBuffer() for the
   * two-tier write path.
   *
   * @param compact_index Index into the compact matrix
   * @param vector Pointer to vector data
   */
  void AddVector(size_t compact_index, const float* vector);

  /**
   * @brief Bulk-add vectors to the index (single lock acquisition)
   * @param compact_indices Array of compact indices to register
   * @param vectors Contiguous array of vectors: row i is the vector for compact_indices[i]
   * @param count Number of vectors to add
   * @param dimension Vector dimension
   */
  void BulkAddVectors(const size_t* compact_indices, const float* vectors,
                       size_t count, uint32_t dimension);

  /**
   * @brief Remove a vector from the index
   *
   * Searches both the IVF inverted lists and the write buffer.
   *
   * @param compact_index Index to remove
   */
  void RemoveVector(size_t compact_index);

  /**
   * @brief Search for nearest neighbors using two-tier IVF + buffer
   *
   * Searches the sealed IVF index (nprobe clusters) and the write buffer
   * (brute-force), then merges and deduplicates results by compact_index.
   *
   * @param query_vec Query vector pointer
   * @param query_norm Pre-computed L2 norm of query vector
   * @param matrix Pointer to compact storage matrix
   * @param norms Pointer to pre-computed norms array
   * @param total_count Total number of slots in compact storage
   * @param dimension Vector dimension
   * @param top_k Number of results to return
   * @return Vector of (score, compact_index) pairs sorted by score descending
   */
  std::vector<std::pair<float, size_t>> Search(
      const float* query_vec, float query_norm,
      const float* matrix, const float* norms,
      size_t total_count, uint32_t dimension,
      size_t top_k) const;

  /**
   * @brief Check if the index has been trained
   * @return True if trained
   */
  bool IsTrained() const;

  /**
   * @brief Get number of indexed vectors (IVF inverted lists only)
   * @return Count of vectors in inverted lists
   */
  size_t GetIndexedCount() const;

  /**
   * @brief Get number of clusters
   * @return Number of Voronoi cells
   */
  size_t GetClusterCount() const;

  /**
   * @brief Reset trained state to allow re-training with more data
   */
  void ResetTrained();

  /**
   * @brief Set the number of probes at query time
   * @param nprobe Number of clusters to search (clamped to nlist)
   */
  void SetNprobe(uint32_t nprobe);

  /**
   * @brief Set the number of clusters before training
   * @param nlist Number of Voronoi cells (overrides auto-scaling)
   */
  void SetNlist(uint32_t nlist);

  /**
   * @brief Get the current nprobe value
   * @return Number of clusters searched per query
   */
  uint32_t GetNprobe() const;

  // =========================================================================
  // Write buffer operations (two-tier architecture)
  // =========================================================================

  /**
   * @brief Append a vector to the write buffer (fast path)
   *
   * Takes only buffer_mutex_ (does not block IVF search or training).
   * The vector data is copied into the buffer's contiguous storage.
   *
   * @param compact_index Index in compact storage
   * @param vector Pointer to vector data (dimension_ floats)
   */
  void AppendToBuffer(size_t compact_index, const float* vector);

  /**
   * @brief Get the number of vectors in the write buffer
   * @return Buffer size
   */
  size_t GetBufferSize() const;

  /**
   * @brief Check if the write buffer has reached the seal threshold
   * @return True if buffer size >= seal_threshold
   */
  bool NeedsSeal() const;

  /**
   * @brief Seal the write buffer by merging entries into the IVF index
   *
   * If the IVF index is trained, assigns each buffer entry to its nearest
   * centroid and appends to the corresponding inverted list. If not trained,
   * the buffer is left as-is (entries will be included in the next Train call).
   *
   * Takes exclusive locks on both buffer_mutex_ and mutex_.
   */
  void SealBuffer();

  /**
   * @brief Search the write buffer with brute-force scan
   *
   * Takes shared lock on buffer_mutex_. Uses SIMD distance functions
   * with prefetching for optimal throughput.
   *
   * @param query_vec Query vector pointer
   * @param query_norm Pre-computed L2 norm of query vector
   * @param dimension Vector dimension
   * @param top_k Number of results to return
   * @return Vector of (score, compact_index) pairs sorted by score descending
   */
  std::vector<std::pair<float, size_t>> SearchBuffer(
      const float* query_vec, float query_norm,
      uint32_t dimension, size_t top_k) const;

 private:
  /**
   * @brief Run K-means (Lloyd's algorithm) on a sample of vectors
   * @param matrix Full matrix pointer
   * @param sample_indices Indices of vectors to use for training
   * @param sample_size Number of training samples
   * @param dim Vector dimension
   */
  void KMeansTrain(const float* matrix, const size_t* sample_indices,
                   size_t sample_size, uint32_t dim);

  /**
   * @brief Find the nearest centroid for a vector
   * @param vec Pointer to vector data
   * @return Index of nearest centroid
   */
  size_t FindNearestCentroid(const float* vec) const;

  /**
   * @brief Find the nprobe nearest centroids for a vector
   * @param vec Pointer to vector data
   * @param nprobe Number of centroids to return
   * @return Vector of centroid indices sorted by distance (nearest first)
   */
  std::vector<size_t> FindNearestCentroids(const float* vec, uint32_t nprobe) const;

  Config config_;
  uint32_t dimension_;
  bool trained_ = false;

  /// Centroids: nlist x dimension contiguous matrix
  std::vector<float> centroids_;
  /// Pre-computed L2 norms of centroids
  std::vector<float> centroid_norms_;

  /// Inverted lists: cluster_id -> list of compact indices
  std::vector<std::vector<size_t>> inverted_lists_;

  /// Reader-writer lock for IVF structures (centroids, inverted lists)
  mutable std::shared_mutex mutex_;

  /// True while Train() is executing (prevents concurrent training attempts)
  std::atomic<bool> training_in_progress_{false};

  // =========================================================================
  // Write buffer state
  // =========================================================================

  /// Compact indices of vectors in the write buffer
  std::vector<size_t> buffer_indices_;

  /// Contiguous vector data for buffer entries [n x dimension_]
  std::vector<float> buffer_vectors_;

  /// Separate reader-writer lock for write buffer (independent of mutex_)
  mutable std::shared_mutex buffer_mutex_;
};

}  // namespace nvecd::vectors
