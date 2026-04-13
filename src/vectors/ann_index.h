/**
 * @file ann_index.h
 * @brief Abstract interface for approximate nearest neighbor indices
 *
 * Defines the AnnIndex abstract class that provides a common interface
 * for different ANN index implementations (HNSW, IVF, flat).
 * All implementations store compact_index values from VectorStore and
 * use DistanceFunc for similarity computation.
 */

#pragma once

#include <cstdint>
#include <iostream>
#include <utility>
#include <vector>

#include "utils/error.h"
#include "utils/expected.h"

namespace nvecd::vectors {

/// Distance function type: computes similarity between two vectors.
/// Higher return values indicate greater similarity.
/// Parameters: (query, candidate, dimension)
using DistanceFunc = float (*)(const float*, const float*, uint32_t);

/**
 * @brief Abstract base class for approximate nearest neighbor indices
 *
 * Provides a unified interface for ANN index implementations.
 * Indices store compact_index values (uint32_t) from VectorStore
 * and support add, delete, search, and rebuild operations.
 *
 * Thread-safety requirements:
 * - Implementations must be safe for concurrent Search calls
 * - Add/MarkDeleted/Rebuild require exclusive access (caller's responsibility)
 */
class AnnIndex {
 public:
  virtual ~AnnIndex() = default;

  /**
   * @brief Add a vector to the index
   * @param compact_index Index in VectorStore's compact storage
   * @param vector Pointer to vector data (dimension floats)
   */
  virtual void Add(uint32_t compact_index, const float* vector) = 0;

  /**
   * @brief Mark a vector as deleted (excluded from search results)
   * @param compact_index Index to mark as deleted
   */
  virtual void MarkDeleted(uint32_t compact_index) = 0;

  /**
   * @brief Search for top-k nearest neighbors
   * @param query Pointer to query vector data
   * @param top_k Number of results to return
   * @return Vector of (compact_index, score) pairs, sorted by score descending
   */
  virtual std::vector<std::pair<uint32_t, float>> Search(
      const float* query, uint32_t top_k) const = 0;

  /**
   * @brief Rebuild the index from scratch
   * @param all_vectors Pointer to contiguous [count x dimension] float matrix
   * @param count Number of vectors
   * @param dimension Vector dimension
   */
  virtual void Rebuild(const float* all_vectors, uint32_t count,
                       uint32_t dimension) = 0;

  /**
   * @brief Get the number of vectors in the index (excluding deleted)
   * @return Active vector count
   */
  virtual uint32_t Size() const = 0;

  /**
   * @brief Serialize index to output stream
   * @param out Output stream
   * @return Success or error
   */
  virtual utils::Expected<void, utils::Error> Serialize(
      std::ostream& out) const = 0;

  /**
   * @brief Deserialize index from input stream
   * @param in Input stream
   * @return Success or error
   */
  virtual utils::Expected<void, utils::Error> Deserialize(
      std::istream& in) = 0;
};

}  // namespace nvecd::vectors
