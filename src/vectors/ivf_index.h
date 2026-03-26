/**
 * @file ivf_index.h
 * @brief IVF (Inverted File) Approximate Nearest Neighbor index
 *
 * Provides an IVF index for accelerating vector similarity search.
 * Vectors are partitioned into Voronoi cells using K-means clustering,
 * and queries search only the nearest nprobe cells instead of all vectors.
 *
 * Thread-safety:
 * - Train/AddVector/RemoveVector: exclusive lock
 * - Search/IsTrained/GetNprobe: shared lock
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
 * Example:
 * @code
 * IvfIndex::Config cfg;
 * cfg.nlist = 256;
 * cfg.nprobe = 8;
 * IvfIndex index(128, cfg);
 *
 * // Train on existing vectors
 * index.Train(matrix_ptr, valid_indices, num_valid, 128);
 *
 * // Add new vectors after training
 * index.AddVector(compact_idx, vec_ptr);
 *
 * // Search
 * auto results = index.Search(query, query_norm, matrix, norms,
 *                             total_count, 128, 10);
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
   * @brief Add a single vector to the index after training
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
   * @param compact_index Index to remove
   */
  void RemoveVector(size_t compact_index);

  /**
   * @brief Search for nearest neighbors using IVF
   *
   * Finds the nprobe nearest centroids, scans those inverted lists,
   * and returns the top-k results ranked by cosine similarity.
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
   * @brief Get number of indexed vectors
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
   * @brief Get the current nprobe value
   * @return Number of clusters searched per query
   */
  uint32_t GetNprobe() const;

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

  /// Reader-writer lock for thread safety
  mutable std::shared_mutex mutex_;

  /// True while Train() is executing (prevents concurrent training attempts)
  std::atomic<bool> training_in_progress_{false};
};

}  // namespace nvecd::vectors
