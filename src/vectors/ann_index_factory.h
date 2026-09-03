/**
 * @file ann_index_factory.h
 * @brief Single construction point for the configured ANN implementation
 *
 * Every AnnIndex implementation is built here so that the mapping from
 * `similarity.index_type` to a concrete index, and the per-implementation
 * wiring that goes with it, exists once. The dispatch is a switch without a
 * `default:` label, and the enumerator count is asserted, so a new
 * implementation cannot be added without being routed through this function
 * and run against the shared ANN contract test suite.
 */

#pragma once

#include <memory>
#include <optional>
#include <string>

#include "vectors/ann_index.h"
#include "vectors/distance.h"
#include "vectors/hnsw_index.h"
#include "vectors/ivf_ann_adapter.h"
#include "vectors/ivf_index.h"
#include "vectors/vector_store.h"

namespace nvecd::vectors {

/**
 * @brief Everything the ANN implementations need at construction time
 *
 * The per-implementation configs are carried side by side; the factory reads
 * only the ones belonging to the selected type.
 */
struct AnnIndexOptions {
  /// Provisional dimension. The index is rebound to the store's real dimension
  /// by the first Rebuild, so this is a pre-sizing hint, not an enforced value.
  uint32_t dimension = 0;
  /// Metric name shared with the brute-force kernel ("cosine", "dot", "l2").
  std::string distance_metric;
  HnswIndex::Config hnsw;
  /// IVF tuning. The metric field is overwritten from @ref distance_metric so
  /// IVF ranking cannot drift from the metric flat and HNSW use.
  IvfIndex::Config ivf;
  /// Store the IVF adapter reads candidate vectors from; required for kIvf.
  VectorStore* vector_store = nullptr;
};

/**
 * @brief Parse a configured index type name
 * @param name Value of `similarity.index_type`
 * @return The matching type, or nullopt for an unrecognised name
 */
inline std::optional<AnnIndexType> AnnIndexTypeFromString(const std::string& name) {
  if (name == "flat") {
    return AnnIndexType::kFlat;
  }
  if (name == "hnsw") {
    return AnnIndexType::kHnsw;
  }
  if (name == "ivf") {
    return AnnIndexType::kIvf;
  }
  return std::nullopt;
}

/**
 * @brief Canonical configuration name for an index type
 * @param type Index type
 * @return The `similarity.index_type` spelling
 */
inline std::string AnnIndexTypeName(AnnIndexType type) {
  switch (type) {
    case AnnIndexType::kFlat:
      return "flat";
    case AnnIndexType::kHnsw:
      return "hnsw";
    case AnnIndexType::kIvf:
      return "ivf";
    case AnnIndexType::kCount:
      break;
  }
  return "flat";
}

/**
 * @brief Construct the ANN index for the selected type
 *
 * @param type Implementation to construct
 * @param options Construction inputs; only the fields of @p type are read
 * @return The index, or nullptr for kFlat (brute-force needs no index object)
 *         and for kIvf without a vector store
 */
inline std::unique_ptr<AnnIndex> MakeAnnIndex(AnnIndexType type, const AnnIndexOptions& options) {
  static_assert(static_cast<int>(AnnIndexType::kCount) == 3,
                "A new AnnIndexType must be constructed here and added to AnnImplementations in "
                "tests/vectors/ann_contract_test.cpp so the shared contract suite covers it.");

  switch (type) {
    case AnnIndexType::kFlat:
      return nullptr;
    case AnnIndexType::kHnsw:
      return std::make_unique<HnswIndex>(options.dimension, GetDistanceFunc(options.distance_metric), options.hnsw);
    case AnnIndexType::kIvf: {
      if (options.vector_store == nullptr) {
        return nullptr;
      }
      IvfIndex::Config ivf_config = options.ivf;
      ivf_config.metric = IvfMetricFromString(options.distance_metric);
      auto ivf = std::make_unique<IvfIndex>(options.dimension, ivf_config);
      return std::make_unique<IvfAnnAdapter>(std::move(ivf), options.vector_store, options.dimension);
    }
    case AnnIndexType::kCount:
      break;
  }
  return nullptr;
}

}  // namespace nvecd::vectors
