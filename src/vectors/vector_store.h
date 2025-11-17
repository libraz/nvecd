/**
 * @file vector_store.h
 * @brief Vector storage and retrieval with dimension validation
 *
 * Thread-safe storage for high-dimensional vectors with automatic
 * dimension validation and optional normalization.
 */

#pragma once

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
  Vector(std::vector<float> data_, bool normalized_ = false)
      : data(std::move(data_)), normalized(normalized_) {}

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
  size_t vector_count = 0;    ///< Number of stored vectors
  size_t dimension = 0;       ///< Vector dimension
  size_t memory_bytes = 0;    ///< Estimated memory usage in bytes
};

/**
 * @brief Thread-safe vector storage
 *
 * Stores vectors with string IDs and enforces consistent dimensions.
 * Supports concurrent reads and exclusive writes.
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
  explicit VectorStore(const config::VectorsConfig& config);

  /**
   * @brief Store a vector with ID
   *
   * If dimension is not yet set (first vector), uses the dimension of this vector.
   * Otherwise, validates that dimension matches existing vectors.
   *
   * @param id Vector ID (e.g., item ID)
   * @param vec Vector data
   * @param normalize Whether to normalize the vector to unit length
   * @return Expected<void, Error> Success or error
   */
  utils::Expected<void, utils::Error> SetVector(const std::string& id,
                                                 const std::vector<float>& vec,
                                                 bool normalize = false);

  /**
   * @brief Retrieve a vector by ID
   *
   * @param id Vector ID
   * @return Optional vector (nullopt if not found)
   */
  std::optional<Vector> GetVector(const std::string& id) const;

  /**
   * @brief Delete a vector by ID
   *
   * @param id Vector ID
   * @return True if deleted, false if not found
   */
  bool DeleteVector(const std::string& id);

  /**
   * @brief Check if a vector exists
   *
   * @param id Vector ID
   * @return True if vector exists
   */
  bool HasVector(const std::string& id) const;

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
  size_t GetDimension() const { return dimension_; }

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

 private:
  config::VectorsConfig config_;  ///< Configuration

  mutable std::shared_mutex mutex_;           ///< Reader-writer lock
  std::unordered_map<std::string, Vector> vectors_;  ///< ID -> Vector mapping
  size_t dimension_ = 0;                      ///< Fixed dimension (0 = not set)
};

}  // namespace nvecd::vectors
