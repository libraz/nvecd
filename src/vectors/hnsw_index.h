/**
 * @file hnsw_index.h
 * @brief HNSW (Hierarchical Navigable Small World) approximate nearest neighbor index
 *
 * Implements the HNSW algorithm for fast approximate nearest neighbor search.
 * Multi-layer graph structure where layer 0 is the densest (2*M connections)
 * and higher layers have M connections each.
 *
 * Key properties:
 * - O(log N) search complexity
 * - High recall (>0.95 at typical settings)
 * - Supports incremental insertion and logical deletion
 *
 * Thread-safety:
 * - Search: concurrent reads allowed (shared_mutex)
 * - Add/MarkDeleted: exclusive lock required
 * - Rebuild: exclusive lock required
 *
 * Reference: "Efficient and robust approximate nearest neighbor search using
 * Hierarchical Navigable Small World graphs" (Malkov & Yashunin, 2018)
 */

#pragma once

#include <cstdint>
#include <iostream>
#include <memory>
#include <random>
#include <shared_mutex>
#include <vector>

#include "utils/error.h"
#include "utils/expected.h"
#include "vectors/ann_index.h"

namespace nvecd::vectors {

/**
 * @brief HNSW index for approximate nearest neighbor search
 */
class HnswIndex : public AnnIndex {
 public:
  /**
   * @brief HNSW configuration parameters
   */
  struct Config {
    uint32_t m = 16;                ///< Number of connections per node (upper layers)
    uint32_t ef_construction = 200; ///< Search width during construction
    uint32_t ef_search = 50;        ///< Search width during query (higher = better recall)
    uint32_t max_elements = 0;      ///< Pre-allocate capacity (0 = grow dynamically)
  };

  /**
   * @brief Construct HNSW index
   * @param dimension Vector dimension
   * @param distance_func Distance function (higher = more similar)
   * @param config HNSW configuration
   */
  HnswIndex(uint32_t dimension, DistanceFunc distance_func,
            const Config& config);

  ~HnswIndex() override = default;

  // Non-copyable
  HnswIndex(const HnswIndex&) = delete;
  HnswIndex& operator=(const HnswIndex&) = delete;

  // Movable
  HnswIndex(HnswIndex&&) noexcept;
  HnswIndex& operator=(HnswIndex&&) noexcept;

  // =========================================================================
  // AnnIndex interface
  // =========================================================================

  void Add(uint32_t compact_index, const float* vector) override;
  void MarkDeleted(uint32_t compact_index) override;
  std::vector<std::pair<uint32_t, float>> Search(
      const float* query, uint32_t top_k) const override;
  void Rebuild(const float* all_vectors, uint32_t count,
               uint32_t dimension) override;
  uint32_t Size() const override;
  utils::Expected<void, utils::Error> Serialize(
      std::ostream& out) const override;
  utils::Expected<void, utils::Error> Deserialize(std::istream& in) override;

  // =========================================================================
  // HNSW-specific methods
  // =========================================================================

  /**
   * @brief Set search width (ef) for queries
   * @param ef_search New ef_search value (clamped to >= top_k during search)
   */
  void SetEfSearch(uint32_t ef_search);

  /**
   * @brief Get current ef_search value
   */
  uint32_t GetEfSearch() const;

  /**
   * @brief Get the maximum layer in the graph
   */
  uint32_t GetMaxLevel() const;

  /**
   * @brief Get total number of nodes (including deleted)
   */
  uint32_t GetNodeCount() const;

 private:
  /// Node in the HNSW graph
  struct Node {
    uint32_t compact_index = 0;  ///< VectorStore compact index
    uint32_t level = 0;          ///< Maximum layer this node exists in
    bool deleted = false;        ///< Logical deletion flag
    /// Neighbors per level: neighbors[l] = list of internal node IDs
    std::vector<std::vector<uint32_t>> neighbors;
  };

  /**
   * @brief Select a random level for a new node
   *
   * Level is drawn from geometric distribution: P(level=l) = (1/m_l)^l
   * where m_l = 1/ln(M). This ensures exponentially fewer nodes at higher levels.
   */
  uint32_t RandomLevel();

  /**
   * @brief Greedy search from entry point at a single layer
   *
   * Returns the ef closest nodes to the query at the given layer.
   *
   * @param query Query vector
   * @param entry_node Starting node (internal ID)
   * @param layer Layer to search
   * @param ef Number of candidates to track
   * @return Vector of (distance, internal_node_id) pairs, sorted ascending by distance
   */
  std::vector<std::pair<float, uint32_t>> SearchLayer(
      const float* query, uint32_t entry_node, uint32_t layer,
      uint32_t ef) const;

  /**
   * @brief Select neighbors using simple heuristic
   *
   * From a candidate set, select up to max_count neighbors.
   * Uses the "simple" selection (closest neighbors).
   *
   * @param candidates (distance, node_id) pairs
   * @param max_count Maximum number of neighbors to select
   * @return Selected neighbor node IDs
   */
  std::vector<uint32_t> SelectNeighbors(
      const std::vector<std::pair<float, uint32_t>>& candidates,
      uint32_t max_count) const;

  /**
   * @brief Get the vector data for an internal node
   */
  const float* GetNodeVector(uint32_t internal_id) const;

  /**
   * @brief Compute distance between query and an internal node
   */
  float ComputeDistance(const float* query, uint32_t internal_id) const;

  /**
   * @brief Maximum number of neighbors at a given level
   */
  uint32_t MaxNeighbors(uint32_t level) const;

  Config config_;
  uint32_t dimension_;
  DistanceFunc distance_func_;

  /// All nodes in the graph (indexed by internal ID)
  std::vector<Node> nodes_;

  /// Vector data storage: [node_count x dimension] contiguous
  std::vector<float> vectors_;

  /// Map from compact_index to internal node ID
  std::vector<uint32_t> compact_to_internal_;

  /// Entry point node (internal ID), UINT32_MAX if empty
  uint32_t entry_point_ = UINT32_MAX;

  /// Maximum level in the current graph
  uint32_t max_level_ = 0;

  /// Number of active (non-deleted) nodes
  uint32_t active_count_ = 0;

  /// Level multiplier: 1/ln(M)
  double level_mult_ = 0.0;

  /// Random number generator for level selection
  mutable std::mt19937 rng_;

  /// Reader-writer lock
  mutable std::shared_mutex mutex_;
};

}  // namespace nvecd::vectors
