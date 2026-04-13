/**
 * @file ivf_ann_adapter.h
 * @brief Adapter that wraps IvfIndex behind the AnnIndex interface
 *
 * IvfIndex has a different API shape (requires external matrix/norms
 * for Search), so this adapter bridges the gap by holding a reference
 * to VectorStore for the data it needs.
 *
 * This allows SimilarityEngine to use IvfIndex through the same
 * AnnIndex interface as HnswIndex.
 */

#pragma once

#include <cstdint>
#include <iostream>
#include <memory>
#include <vector>

#include "utils/error.h"
#include "utils/expected.h"
#include "vectors/ann_index.h"
#include "vectors/distance_simd.h"
#include "vectors/ivf_index.h"
#include "vectors/vector_store.h"

namespace nvecd::vectors {

/**
 * @brief AnnIndex adapter for IvfIndex
 *
 * Wraps IvfIndex to conform to the AnnIndex interface.
 * Requires a non-owning reference to VectorStore for Search operations
 * (IvfIndex needs external matrix/norms data).
 */
class IvfAnnAdapter : public AnnIndex {
 public:
  /**
   * @brief Construct adapter
   * @param ivf The underlying IvfIndex (takes ownership)
   * @param vector_store Non-owning reference to VectorStore (must outlive this object)
   * @param dimension Vector dimension
   */
  IvfAnnAdapter(std::unique_ptr<IvfIndex> ivf, VectorStore* vector_store,
                uint32_t dimension)
      : ivf_(std::move(ivf)),
        vector_store_(vector_store),
        dimension_(dimension) {}

  void Add(uint32_t compact_index, const float* vector) override {
    ivf_->AppendToBuffer(static_cast<size_t>(compact_index), vector);
  }

  void MarkDeleted(uint32_t compact_index) override {
    ivf_->RemoveVector(static_cast<size_t>(compact_index));
  }

  std::vector<std::pair<uint32_t, float>> Search(
      const float* query, uint32_t top_k) const override {
    auto snap = vector_store_->GetCompactSnapshot();
    if (snap.count == 0 || snap.matrix == nullptr) {
      return {};
    }

    float query_norm =
        simd::GetOptimalImpl().l2_norm(query, static_cast<size_t>(dimension_));

    auto ivf_results = ivf_->Search(
        query, query_norm, snap.matrix, snap.norms, snap.count, dimension_,
        static_cast<size_t>(top_k));

    std::vector<std::pair<uint32_t, float>> results;
    results.reserve(ivf_results.size());
    for (const auto& [score, idx] : ivf_results) {
      results.push_back({static_cast<uint32_t>(idx), score});
    }
    return results;
  }

  void Rebuild(const float* all_vectors, uint32_t count,
               uint32_t dimension) override {
    // Build valid indices
    std::vector<size_t> valid_indices;
    valid_indices.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
      valid_indices.push_back(i);
    }
    ivf_->ResetTrained();
    ivf_->Train(all_vectors, valid_indices.data(), count, dimension);
  }

  uint32_t Size() const override {
    return static_cast<uint32_t>(ivf_->GetIndexedCount() +
                                 ivf_->GetBufferSize());
  }

  utils::Expected<void, utils::Error> Serialize(
      std::ostream& /*out*/) const override {
    // IVF serialization is handled by snapshot_format directly
    return utils::MakeUnexpected(
        utils::MakeError(utils::ErrorCode::kNotImplemented,
                         "IVF serialization via AnnIndex not supported"));
  }

  utils::Expected<void, utils::Error> Deserialize(
      std::istream& /*in*/) override {
    return utils::MakeUnexpected(
        utils::MakeError(utils::ErrorCode::kNotImplemented,
                         "IVF deserialization via AnnIndex not supported"));
  }

  /// @brief Access the underlying IvfIndex directly
  IvfIndex* GetIvfIndex() { return ivf_.get(); }
  const IvfIndex* GetIvfIndex() const { return ivf_.get(); }

  /// @brief Check if the underlying IVF is trained
  bool IsTrained() const { return ivf_->IsTrained(); }

  /// @brief Check if buffer needs sealing
  bool NeedsSeal() const { return ivf_->NeedsSeal(); }

  /// @brief Seal the write buffer
  void SealBuffer() { ivf_->SealBuffer(); }

 private:
  std::unique_ptr<IvfIndex> ivf_;
  VectorStore* vector_store_;
  uint32_t dimension_;
};

}  // namespace nvecd::vectors
