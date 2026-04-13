/**
 * @file vector_store.h
 * @brief Vector storage and retrieval with dimension validation
 *
 * Thread-safe storage for high-dimensional vectors with automatic
 * dimension validation and optional normalization.
 *
 * Uses compact contiguous storage as the single source of truth
 * for optimal search performance and reduced memory usage.
 */

#pragma once

#include <atomic>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "config/config.h"
#include "utils/error.h"
#include "utils/expected.h"

namespace nvecd::vectors {

/**
 * @brief Vector data with metadata
 */
struct Vector {
  std::vector<float> data;  ///< Vector components
  bool normalized = false;  ///< Whether vector is L2-normalized

  Vector() = default;
  Vector(std::vector<float> data_, bool normalized_ = false) : data(std::move(data_)), normalized(normalized_) {}

  /**
   * @brief Get vector dimension
   * @return Number of components
   */
  size_t Dimension() const { return data.size(); }
};

/**
 * @brief Vector store statistics
 */
struct VectorStoreStatistics {
  size_t vector_count = 0;  ///< Number of stored vectors
  size_t dimension = 0;     ///< Vector dimension
  size_t memory_bytes = 0;  ///< Estimated memory usage in bytes
};

/**
 * @brief Thread-safe vector storage
 *
 * Stores vectors with string IDs and enforces consistent dimensions.
 * Supports concurrent reads and exclusive writes.
 *
 * Uses compact contiguous storage (matrix_ + norms_) as the single
 * source of truth. Deleted entries are tracked with tombstones and
 * defragmented when fragmentation exceeds 25%.
 *
 * Thread-safety:
 * - Multiple concurrent readers (GetVector, GetAllIds, etc.)
 * - Exclusive writer (SetVector, DeleteVector)
 *
 * Example:
 * @code
 * VectorStore store(config);
 * std::vector<float> vec = {0.1f, 0.2f, 0.3f};
 * auto result = store.SetVector("item1", vec);
 * if (result) {
 *   auto retrieved = store.GetVector("item1");
 * }
 * @endcode
 */
class VectorStore {
 public:
  /**
   * @brief Construct vector store with configuration
   * @param config Vector store configuration
   */
  explicit VectorStore(config::VectorsConfig config);

  /**
   * @brief Store a vector with ID
   *
   * If dimension is not yet set (first vector), uses the dimension of this vector.
   * Otherwise, validates that dimension matches existing vectors.
   *
   * @param vector_id Vector ID (e.g., item ID)
   * @param vec Vector data
   * @param normalize Whether to normalize the vector to unit length
   * @return Expected<void, Error> Success or error
   */
  utils::Expected<void, utils::Error> SetVector(const std::string& vector_id, const std::vector<float>& vec,
                                                bool normalize = false);

  /**
   * @brief Retrieve a vector by ID
   *
   * @param vector_id Vector ID
   * @return Optional vector (nullopt if not found)
   */
  std::optional<Vector> GetVector(const std::string& vector_id) const;

  /**
   * @brief Delete a vector by ID
   *
   * @param vector_id Vector ID
   * @return True if deleted, false if not found
   */
  bool DeleteVector(const std::string& vector_id);

  /**
   * @brief Check if a vector exists
   *
   * @param vector_id Vector ID
   * @return True if vector exists
   */
  bool HasVector(const std::string& vector_id) const;

  /**
   * @brief Get all vector IDs
   * @return Vector of all IDs
   */
  std::vector<std::string> GetAllIds() const;

  /**
   * @brief Get number of stored vectors
   * @return Vector count
   */
  size_t GetVectorCount() const;

  /**
   * @brief Get the dimension of stored vectors
   *
   * Returns 0 if no vectors have been stored yet, or the dimension
   * of the first vector stored.
   *
   * @return Vector dimension (0 if empty)
   */
  size_t GetDimension() const { return dimension_.load(std::memory_order_acquire); }

  /**
   * @brief Clear all vectors
   */
  void Clear();

  /**
   * @brief Get vector store statistics
   * @return Statistics snapshot
   */
  VectorStoreStatistics GetStatistics() const;

  /**
   * @brief Get estimated memory usage in bytes
   * @return Estimated memory usage
   */
  size_t MemoryUsage() const;

  /**
   * @brief Get number of vectors in compact storage (including tombstones)
   * @return Total number of slots in compact matrix
   */
  size_t GetCompactCount() const;

  /**
   * @brief Get pointer to vector data at given index in compact storage
   *
   * Caller MUST hold read lock via AcquireReadLock().
   *
   * @param idx Row index in compact matrix
   * @return Pointer to first element of the vector
   */
  const float* GetMatrixRow(size_t idx) const;

  /**
   * @brief Get raw pointer to contiguous matrix data (caller must hold read lock)
   * @return Pointer to matrix data, or nullptr if empty
   */
  const float* GetMatrixData() const;

  /**
   * @brief Get number of rows in the matrix (caller must hold read lock)
   * @return Row count (includes tombstones)
   */
  size_t GetMatrixCount() const;

  /**
   * @brief Get pre-computed L2 norm for vector at given index
   *
   * Caller MUST hold read lock via AcquireReadLock().
   *
   * @param idx Row index in compact matrix
   * @return L2 norm of the vector
   */
  float GetNorm(size_t idx) const;

  /**
   * @brief Get ID for vector at given index in compact storage
   *
   * Caller MUST hold read lock via AcquireReadLock().
   *
   * @param idx Row index in compact matrix
   * @return Reference to the vector ID string
   */
  const std::string& GetIdByIndex(size_t idx) const;

  /**
   * @brief Acquire read lock for external batch operations
   * @return Shared lock guard
   */
  std::shared_lock<std::shared_mutex> AcquireReadLock() const;

  /**
   * @brief Acquire write lock for snapshot consistency
   * @return Unique lock guard
   */
  std::unique_lock<std::shared_mutex> AcquireWriteLock();

  /**
   * @brief Snapshot of compact storage for lock-free read access
   *
   * The caller should hold a read lock while using this snapshot.
   */
  struct CompactSnapshot {
    const float* matrix = nullptr;  ///< Pointer to contiguous matrix
    const float* norms = nullptr;   ///< Pointer to norm array
    size_t count = 0;               ///< Number of vectors (including tombstones)
    size_t dim = 0;                 ///< Vector dimension
    const std::unordered_map<std::string, size_t>* id_to_idx = nullptr;
    const std::vector<std::string>* idx_to_id = nullptr;
  };

  /**
   * @brief Get a snapshot of compact storage under read lock
   *
   * Acquires read lock briefly to copy pointers, then releases.
   *
   * @return CompactSnapshot with pointers to compact data, or empty if no data
   */
  CompactSnapshot GetCompactSnapshot() const;

  /**
   * @brief Get index for a given ID in compact storage
   * @param id Vector ID to look up
   * @return Index in compact matrix, or nullopt if not found
   */
  std::optional<size_t> GetCompactIndex(const std::string& id) const;

  /**
   * @brief Check if slot at given index is deleted (tombstone)
   * @param idx Row index in compact matrix
   * @return True if the slot is deleted
   */
  bool IsDeleted(size_t idx) const;

  /**
   * @brief Remove tombstones when fragmentation exceeds threshold
   *
   * Rebuilds compact storage by copying only non-deleted entries.
   * Called automatically when tombstone ratio exceeds 25%.
   * Thread-safe: acquires exclusive lock internally.
   */
  void Defragment();

 private:
  /// @brief Internal defragment that assumes caller holds unique_lock
  void DefragmentLocked();

  /// @brief Internal memory usage calculation that assumes caller holds lock
  size_t MemoryUsageLocked() const;
  config::VectorsConfig config_;  ///< Configuration

  mutable std::shared_mutex mutex_;   ///< Reader-writer lock
  std::atomic<size_t> dimension_{0};  ///< Fixed dimension (0 = not set)

  // Compact storage (single source of truth)
  std::vector<float> matrix_;                          ///< [n x dim] contiguous float array
  std::vector<float> norms_;                           ///< [n] pre-computed L2 norms
  std::unordered_map<std::string, size_t> id_to_idx_;  ///< ID -> row index
  std::vector<std::string> idx_to_id_;                 ///< row index -> ID

  // Tombstone tracking
  std::vector<bool> deleted_;   ///< Tombstone flags per slot
  size_t active_count_ = 0;     ///< Number of non-deleted vectors
  size_t tombstone_count_ = 0;  ///< Number of deleted slots
};

}  // namespace nvecd::vectors
