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
 * @brief Selects which ANN implementation backs similarity search
 *
 * Mirrors the `similarity.index_type` configuration key. Dispatch on this enum
 * is written without a `default:` label so the compiler reports every site that
 * has not been taught about a new implementation.
 */
enum class AnnIndexType : uint8_t {
  kFlat = 0,  ///< Brute-force scan; no AnnIndex object is constructed
  kHnsw = 1,  ///< Hierarchical navigable small world graph
  kIvf = 2,   ///< Inverted file (k-means cells) behind IvfAnnAdapter
  /// Sentinel holding the number of implementations. Adding an enumerator above
  /// changes its value and trips the static assertions in ann_index_factory.h
  /// and in the shared ANN contract test suite, so a new implementation cannot
  /// be introduced without being routed through MakeAnnIndex and held to the
  /// same contract as the existing ones.
  kCount
};

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
 *
 * Behavioural contract shared by every implementation. Each clause is fixed by
 * the type-parameterized suite in tests/vectors/ann_contract_test.cpp, which
 * runs against every implementation, so an implementation that satisfies only
 * part of the contract turns the tests red:
 * - After Rebuild(_, _, dimension), Dimension() == dimension on every exit
 *   path, including the count == 0 rebind that adopts a dimension without
 *   inserting anything.
 * - Search with top_k == 0 returns an empty result set and performs no
 *   operation on an empty container.
 * - Search never returns a compact_index that was passed to MarkDeleted and not
 *   re-Added since.
 * - Size() equals the number of live compact indices; MarkDeleted on an unknown
 *   or already-deleted index leaves it unchanged and never wraps.
 * - A zero-norm candidate scores 0.0 under cosine and takes part in ranking, as
 *   it does in the brute-force kernel, so switching implementations changes the
 *   ranking within the recall envelope but not the membership of the result.
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
   * @param top_k Number of results to return (0 returns an empty result set)
   * @return Vector of (compact_index, score) pairs, sorted by score descending
   */
  virtual std::vector<std::pair<uint32_t, float>> Search(const float* query, uint32_t top_k) const = 0;

  /**
   * @brief Rebuild the index from scratch
   *
   * Adopts @p dimension on every exit path, including `count == 0`, which is
   * how the caller rebinds a still-empty index from the provisional configured
   * dimension to the dimension the vector store actually holds. Leaving the
   * previous dimension in place there makes the next insert read past the
   * caller's vector buffer.
   *
   * @param all_vectors Pointer to contiguous [count x dimension] float matrix
   * @param count Number of vectors
   * @param dimension Vector dimension
   */
  virtual void Rebuild(const float* all_vectors, uint32_t count, uint32_t dimension) = 0;

  /**
   * @brief Get the vector dimension the index is currently bound to
   * @return Dimension used for the search stride and for incoming vectors
   */
  virtual uint32_t Dimension() const = 0;

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
  virtual utils::Expected<void, utils::Error> Serialize(std::ostream& out) const = 0;

  /**
   * @brief Deserialize index from input stream
   * @param in Input stream
   * @return Success or error
   */
  virtual utils::Expected<void, utils::Error> Deserialize(std::istream& in) = 0;
};

}  // namespace nvecd::vectors
