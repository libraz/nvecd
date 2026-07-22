/**
 * @file vector_store.cpp
 * @brief Vector store implementation with compact single-source-of-truth storage
 */

#include "vectors/vector_store.h"

#include <algorithm>
#include <cmath>

#include "utils/error.h"
#include "utils/structured_log.h"
#include "vectors/distance.h"
#include "vectors/distance_simd.h"

namespace nvecd::vectors {

namespace {
constexpr size_t kMaxVectorDimension = 4096;
}

VectorStore::VectorStore(config::VectorsConfig config) : config_(std::move(config)) {}

utils::Expected<void, utils::Error> VectorStore::SetVector(const std::string& vector_id, const std::vector<float>& vec,
                                                           bool normalize) {
  // Validate inputs
  if (vector_id.empty()) {
    auto error = utils::MakeError(utils::ErrorCode::kInvalidArgument, "ID cannot be empty");
    utils::LogVectorStoreError("set_vector", vector_id, static_cast<int>(vec.size()), error.message());
    return utils::MakeUnexpected(error);
  }

  if (vec.empty()) {
    auto error = utils::MakeError(utils::ErrorCode::kInvalidArgument, "Vector cannot be empty");
    utils::LogVectorStoreError("set_vector", vector_id, static_cast<int>(vec.size()), error.message());
    return utils::MakeUnexpected(error);
  }
  if (vec.size() > kMaxVectorDimension) {
    auto error = utils::MakeError(utils::ErrorCode::kVectorDimensionMismatch,
                                  "Vector dimension exceeds maximum of " + std::to_string(kMaxVectorDimension));
    utils::LogVectorStoreError("set_vector", vector_id, static_cast<int>(vec.size()), error.message());
    return utils::MakeUnexpected(error);
  }

  // Prepare vector (normalize if requested)
  std::vector<float> data = vec;

  if (normalize) {
    if (!Normalize(data)) {
      auto error = utils::MakeError(utils::ErrorCode::kInvalidArgument, "Cannot normalize zero vector");
      utils::LogVectorStoreError("set_vector", vector_id, static_cast<int>(vec.size()), error.message());
      return utils::MakeUnexpected(error);
    }
  }

  // Store vector in compact storage
  {
    std::unique_lock lock(mutex_);

    // Set dimension if this is the first vector
    size_t current_dim = dimension_.load(std::memory_order_relaxed);
    if (current_dim == 0) {
      dimension_.store(data.size(), std::memory_order_release);
      current_dim = data.size();
    }

    // Validate dimension
    if (data.size() != current_dim) {
      auto error = utils::MakeError(utils::ErrorCode::kVectorDimensionMismatch,
                                    "Vector dimension mismatch: expected " + std::to_string(current_dim) + ", got " +
                                        std::to_string(data.size()));
      utils::LogVectorStoreError("set_vector", vector_id, static_cast<int>(data.size()), error.message());
      return utils::MakeUnexpected(error);
    }

    auto it = id_to_idx_.find(vector_id);
    if (it != id_to_idx_.end()) {
      // Existing vector: overwrite in-place
      size_t idx = it->second;
      std::copy(data.begin(), data.end(), matrix_.begin() + static_cast<ptrdiff_t>(idx * current_dim));
      norms_[idx] = simd::GetOptimalImpl().l2_norm(data.data(), data.size());
      normalized_[idx] = normalize;
    } else {
      // New vector: check for tombstone slot to reuse
      size_t idx = idx_to_id_.size();  // Default: append

      if (!free_slots_.empty()) {
        // Deletion records reusable positions, avoiding an O(number of slots)
        // scan on every new vector after a fragmented workload.
        idx = free_slots_.back();
        free_slots_.pop_back();
        deleted_[idx] = false;
        --tombstone_count_;

        std::copy(data.begin(), data.end(), matrix_.begin() + static_cast<ptrdiff_t>(idx * current_dim));
        norms_[idx] = simd::GetOptimalImpl().l2_norm(data.data(), data.size());
        normalized_[idx] = normalize;
        idx_to_id_[idx] = vector_id;
        id_to_idx_[vector_id] = idx;
        ++active_count_;
        return {};
      }

      // Append new slot
      matrix_.resize(matrix_.size() + current_dim);
      std::copy(data.begin(), data.end(), matrix_.begin() + static_cast<ptrdiff_t>(idx * current_dim));
      norms_.push_back(simd::GetOptimalImpl().l2_norm(data.data(), data.size()));
      normalized_.push_back(normalize);
      idx_to_id_.push_back(vector_id);
      deleted_.push_back(false);
      id_to_idx_[vector_id] = idx;
      ++active_count_;
    }
  }

  return {};
}

std::optional<Vector> VectorStore::GetVector(const std::string& vector_id) const {
  std::shared_lock lock(mutex_);

  auto it = id_to_idx_.find(vector_id);
  if (it == id_to_idx_.end()) {
    return std::nullopt;
  }

  size_t idx = it->second;
  size_t dim = dimension_.load(std::memory_order_relaxed);

  // Defensive check
  if (idx < deleted_.size() && deleted_[idx]) {
    return std::nullopt;
  }

  const float* begin = matrix_.data() + idx * dim;
  std::vector<float> data(begin, begin + dim);

  return Vector(std::move(data), normalized_[idx]);
}

bool VectorStore::DeleteVector(const std::string& vector_id) {
  std::unique_lock lock(mutex_);

  auto it = id_to_idx_.find(vector_id);
  if (it == id_to_idx_.end()) {
    return false;
  }

  size_t idx = it->second;
  deleted_[idx] = true;
  free_slots_.push_back(idx);
  id_to_idx_.erase(it);
  --active_count_;
  ++tombstone_count_;

  // Auto-defragment when fragmentation exceeds 25%
  size_t total_slots = idx_to_id_.size();
  if (total_slots > 0 && tombstone_count_ * 4 > total_slots) {
    DefragmentLocked();
  }

  return true;
}

bool VectorStore::HasVector(const std::string& vector_id) const {
  std::shared_lock lock(mutex_);
  return id_to_idx_.count(vector_id) > 0;
}

std::vector<std::string> VectorStore::GetAllIds() const {
  std::shared_lock lock(mutex_);

  std::vector<std::string> ids;
  ids.reserve(active_count_);

  for (const auto& [id, idx] : id_to_idx_) {
    ids.push_back(id);
  }

  return ids;
}

size_t VectorStore::GetVectorCount() const {
  std::shared_lock lock(mutex_);
  return active_count_;
}

void VectorStore::Clear() {
  std::unique_lock lock(mutex_);
  matrix_.clear();
  norms_.clear();
  normalized_.clear();
  id_to_idx_.clear();
  idx_to_id_.clear();
  deleted_.clear();
  free_slots_.clear();
  active_count_ = 0;
  tombstone_count_ = 0;
  dimension_.store(0, std::memory_order_release);
}

VectorStoreStatistics VectorStore::GetStatistics() const {
  std::shared_lock lock(mutex_);

  VectorStoreStatistics stats;
  stats.vector_count = active_count_;
  stats.dimension = dimension_.load(std::memory_order_relaxed);
  stats.memory_bytes = MemoryUsageLocked();

  return stats;
}

size_t VectorStore::MemoryUsage() const {
  std::shared_lock lock(mutex_);
  return MemoryUsageLocked();
}

size_t VectorStore::MemoryUsageLocked() const {
  size_t total = 0;

  // Base object overhead
  total += sizeof(*this);

  // Compact matrix storage
  total += matrix_.capacity() * sizeof(float);

  // Norms array
  total += norms_.capacity() * sizeof(float);
  total += normalized_.capacity() / 8;

  // Deleted flags (std::vector<bool> uses ~1 bit per entry)
  total += deleted_.capacity() / 8;
  total += free_slots_.capacity() * sizeof(size_t);

  // ID strings in idx_to_id_
  for (const auto& id : idx_to_id_) {
    total += sizeof(std::string) + id.capacity();
  }

  // id_to_idx_ hash map overhead
  total += id_to_idx_.bucket_count() * (sizeof(size_t) + sizeof(std::string) + sizeof(void*));

  return total;
}

// ============================================================================
// Compact Storage Access
// ============================================================================

size_t VectorStore::GetCompactCount() const {
  return idx_to_id_.size();
}

const float* VectorStore::GetMatrixRow(size_t idx) const {
  size_t dim = dimension_.load(std::memory_order_relaxed);
  return matrix_.data() + idx * dim;
}

const float* VectorStore::GetMatrixData() const {
  return matrix_.empty() ? nullptr : matrix_.data();
}

size_t VectorStore::GetMatrixCount() const {
  size_t dim = dimension_.load(std::memory_order_relaxed);
  return dim > 0 ? matrix_.size() / dim : 0;
}

float VectorStore::GetNorm(size_t idx) const {
  return norms_[idx];
}

const std::string& VectorStore::GetIdByIndex(size_t idx) const {
  return idx_to_id_[idx];
}

std::shared_lock<std::shared_mutex> VectorStore::AcquireReadLock() const {
  return std::shared_lock<std::shared_mutex>(mutex_);
}

VectorStore::CompactSnapshot VectorStore::GetCompactSnapshot() const {
  // Acquire the read lock and move it into the snapshot so it is held for the
  // snapshot's entire lifetime. This keeps matrix_/norms_/maps from being
  // reallocated or defragmented out from under the consumer.
  std::shared_lock<std::shared_mutex> lock(mutex_);
  CompactSnapshot snap;
  if (matrix_.empty()) {
    // Still move the lock so the Empty() snapshot's contract is uniform;
    // an empty store has nothing to protect but holding the lock is harmless.
    snap.lock = std::move(lock);
    return snap;
  }
  snap.matrix = matrix_.data();
  snap.norms = norms_.data();
  snap.count = idx_to_id_.size();
  snap.dim = dimension_.load(std::memory_order_relaxed);
  snap.id_to_idx = &id_to_idx_;
  snap.idx_to_id = &idx_to_id_;
  snap.deleted = &deleted_;
  snap.lock = std::move(lock);
  return snap;
}

std::optional<size_t> VectorStore::GetCompactIndex(const std::string& id) const {
  // Acquire a read lock: this method is called standalone (not via a snapshot)
  // from request handlers and snapshot serialization, concurrently with
  // SetVector/DeleteVector which mutate id_to_idx_.
  std::shared_lock lock(mutex_);
  auto it = id_to_idx_.find(id);
  if (it == id_to_idx_.end()) {
    return std::nullopt;
  }
  return it->second;
}

bool VectorStore::IsDeleted(size_t idx) const {
  return idx < deleted_.size() && deleted_[idx];
}

void VectorStore::Defragment() {
  std::unique_lock lock(mutex_);
  DefragmentLocked();
}

void VectorStore::DefragmentLocked() {
  size_t dim = dimension_.load(std::memory_order_relaxed);
  if (dim == 0 || active_count_ == 0) {
    matrix_.clear();
    norms_.clear();
    normalized_.clear();
    id_to_idx_.clear();
    idx_to_id_.clear();
    deleted_.clear();
    free_slots_.clear();
    active_count_ = 0;
    tombstone_count_ = 0;
    return;
  }

  size_t new_n = active_count_;
  std::vector<float> new_matrix(new_n * dim);
  std::vector<float> new_norms(new_n);
  std::vector<bool> new_normalized(new_n, false);
  std::unordered_map<std::string, size_t> new_id_to_idx;
  new_id_to_idx.reserve(new_n);
  std::vector<std::string> new_idx_to_id;
  new_idx_to_id.reserve(new_n);
  std::vector<bool> new_deleted(new_n, false);

  size_t new_idx = 0;
  for (size_t old_idx = 0; old_idx < idx_to_id_.size(); ++old_idx) {
    if (deleted_[old_idx]) {
      continue;
    }

    // Copy matrix row
    const float* src = matrix_.data() + old_idx * dim;
    float* dst = new_matrix.data() + new_idx * dim;
    std::copy(src, src + dim, dst);

    new_norms[new_idx] = norms_[old_idx];
    new_normalized[new_idx] = normalized_[old_idx];
    new_idx_to_id.push_back(idx_to_id_[old_idx]);
    new_id_to_idx[idx_to_id_[old_idx]] = new_idx;
    ++new_idx;
  }

  matrix_ = std::move(new_matrix);
  norms_ = std::move(new_norms);
  normalized_ = std::move(new_normalized);
  id_to_idx_ = std::move(new_id_to_idx);
  idx_to_id_ = std::move(new_idx_to_id);
  deleted_ = std::move(new_deleted);
  free_slots_.clear();
  tombstone_count_ = 0;
}

std::unique_lock<std::shared_mutex> VectorStore::AcquireWriteLock() {
  return std::unique_lock<std::shared_mutex>(mutex_);
}

}  // namespace nvecd::vectors
