/**
 * @file tiered_vector_store.h
 * @brief Main/Delta two-tier vector storage for controlled rebuild costs
 *
 * TieredVectorStore separates vector storage into two tiers:
 * - Main: read-optimized, holds the majority of vectors with an HNSW index
 * - Delta: write-optimized, holds recent additions (brute-force search)
 *
 * New vectors are inserted into Delta. When Delta exceeds a threshold,
 * it is merged into Main and the Main index is rebuilt. Search queries
 * both tiers and merges results.
 *
 * Thread-safety:
 * - Multiple concurrent readers (Search)
 * - Exclusive writer (Add, Delete, Update, Merge, Rebuild)
 */

#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "utils/error.h"
#include "utils/expected.h"
#include "vectors/ann_index.h"

namespace nvecd::vectors {

/**
 * @brief Search result from TieredVectorStore
 */
struct TieredSearchResult {
  std::string id;
  float score;
};

/**
 * @brief Two-tier vector storage with Main (read-optimized) and Delta (write-optimized)
 */
class TieredVectorStore {
 public:
  /**
   * @brief Configuration for TieredVectorStore
   */
  struct Config {
    uint32_t delta_merge_threshold = 50000;  ///< Merge delta when it exceeds this count
    float tombstone_ratio_threshold = 0.1F;  ///< Rebuild main when deleted ratio exceeds this
    std::string distance_metric = "cosine";  ///< Distance metric: "cosine", "dot", "l2"
    uint32_t hnsw_m = 16;                    ///< HNSW: connections per node
    uint32_t hnsw_ef_construction = 200;     ///< HNSW: search width during construction
    uint32_t hnsw_ef_search = 50;            ///< HNSW: search width during query
  };

  explicit TieredVectorStore(const Config& config);
  ~TieredVectorStore() = default;

  TieredVectorStore(const TieredVectorStore&) = delete;
  TieredVectorStore& operator=(const TieredVectorStore&) = delete;

  // =========================================================================
  // Core API
  // =========================================================================

  /**
   * @brief Add a vector to the delta store
   * @param id Vector identifier
   * @param vec Vector data (dimension is set on first insert)
   * @return Success or error (dimension mismatch)
   */
  utils::Expected<void, utils::Error> Add(const std::string& id, std::vector<float> vec);

  /**
   * @brief Delete a vector by ID
   * @param id Vector identifier
   * @return Success or kNotFound if ID doesn't exist
   */
  utils::Expected<void, utils::Error> Delete(const std::string& id);

  /**
   * @brief Update a vector (delete old + add new)
   * @param id Vector identifier
   * @param vec New vector data
   * @return Success or error
   */
  utils::Expected<void, utils::Error> Update(const std::string& id, std::vector<float> vec);

  /**
   * @brief Search both tiers and merge results
   * @param query Query vector (must match dimension)
   * @param top_k Number of results to return
   * @return Merged results sorted by score descending
   */
  std::vector<TieredSearchResult> Search(const float* query, uint32_t top_k) const;

  // =========================================================================
  // Merge / Rebuild
  // =========================================================================

  /**
   * @brief Merge delta vectors into main and rebuild main index
   * @return Success or error
   */
  utils::Expected<void, utils::Error> MergeDeltaToMain();

  /**
   * @brief Rebuild main by compacting deleted entries and rebuilding index
   * @return Success or error
   */
  utils::Expected<void, utils::Error> RebuildMain();

  // =========================================================================
  // Accessors
  // =========================================================================

  size_t MainSize() const;
  size_t DeltaSize() const;
  size_t TotalSize() const;
  size_t DeletedCount() const;
  uint32_t GetDimension() const;
  bool NeedsMerge() const;
  bool NeedsRebuild() const;
  bool HasVector(const std::string& id) const;

  /// Check which tier a vector resides in (for testing)
  bool IsInMain(const std::string& id) const;
  bool IsInDelta(const std::string& id) const;

 private:
  enum class StoreLocation { kMain, kDelta };

  struct IdLocation {
    StoreLocation store;
    uint32_t compact_index;
  };

  /// Main tier: read-optimized with ANN index
  struct MainStore {
    std::vector<float> matrix;             ///< [count x dim] contiguous
    std::vector<float> norms;              ///< [count] L2 norms
    std::vector<std::string> ids;          ///< compact_index -> id
    std::unique_ptr<AnnIndex> index;       ///< HNSW index (may be null)
    std::unordered_set<uint32_t> deleted;  ///< Deleted compact indices

    size_t ActiveSize() const { return ids.size() - deleted.size(); }
    uint32_t TotalSlots() const { return static_cast<uint32_t>(ids.size()); }
  };

  /// Delta tier: write-optimized, no index (brute-force search)
  struct DeltaStore {
    std::vector<float> matrix;
    std::vector<float> norms;
    std::vector<std::string> ids;

    size_t Size() const { return ids.size(); }
    void Clear() {
      matrix.clear();
      norms.clear();
      ids.clear();
    }
  };

  // Search helpers (caller holds shared lock)
  std::vector<TieredSearchResult> SearchMain(const float* query, uint32_t top_k) const;
  std::vector<TieredSearchResult> SearchDelta(const float* query, uint32_t top_k) const;
  static std::vector<TieredSearchResult> MergeResults(std::vector<TieredSearchResult>& main_results,
                                                      std::vector<TieredSearchResult>& delta_results, uint32_t top_k);

  /// Remove entry from delta by swap-with-last (O(1) matrix operation)
  void RemoveFromDelta(uint32_t delta_idx);

  /// Rebuild the HNSW index on main_.matrix
  void RebuildMainIndex();

  /// Compute L2 norm of a vector
  static float ComputeNorm(const float* vec, uint32_t dim);

  Config config_;
  uint32_t dimension_ = 0;
  DistanceFunc distance_func_ = nullptr;

  MainStore main_;
  DeltaStore delta_;
  std::unordered_map<std::string, IdLocation> id_location_;

  mutable std::shared_mutex mutex_;
};

}  // namespace nvecd::vectors
