/**
 * @file vector_store.cpp
 * @brief Vector store implementation
 */

#include "vectors/vector_store.h"

#include "utils/error.h"
#include "utils/structured_log.h"
#include "vectors/distance.h"

namespace nvecd::vectors {

VectorStore::VectorStore(const config::VectorsConfig& config)
    : config_(config) {}

utils::Expected<void, utils::Error> VectorStore::SetVector(
    const std::string& id, const std::vector<float>& vec, bool normalize) {
  // Validate inputs
  if (id.empty()) {
    auto error =
        utils::MakeError(utils::ErrorCode::kInvalidArgument, "ID cannot be empty");
    utils::LogVectorStoreError("set_vector", id, vec.size(), error.message());
    return utils::MakeUnexpected(error);
  }

  if (vec.empty()) {
    auto error = utils::MakeError(utils::ErrorCode::kInvalidArgument,
                                  "Vector cannot be empty");
    utils::LogVectorStoreError("set_vector", id, vec.size(), error.message());
    return utils::MakeUnexpected(error);
  }

  // Prepare vector (normalize if requested)
  std::vector<float> data = vec;
  bool is_normalized = false;

  if (normalize) {
    if (!Normalize(data)) {
      auto error = utils::MakeError(utils::ErrorCode::kInvalidArgument,
                                    "Cannot normalize zero vector");
      utils::LogVectorStoreError("set_vector", id, vec.size(),
                                 error.message());
      return utils::MakeUnexpected(error);
    }
    is_normalized = true;
  }

  // Store vector
  {
    std::unique_lock lock(mutex_);

    // Set dimension if this is the first vector
    if (dimension_ == 0) {
      dimension_ = data.size();
    }

    // Validate dimension
    if (data.size() != dimension_) {
      auto error = utils::MakeError(
          utils::ErrorCode::kVectorDimensionMismatch,
          "Vector dimension mismatch: expected " + std::to_string(dimension_) +
              ", got " + std::to_string(data.size()));
      utils::LogVectorStoreError("set_vector", id, data.size(),
                                 error.message());
      return utils::MakeUnexpected(error);
    }

    // Store vector
    vectors_[id] = Vector(std::move(data), is_normalized);
  }

  return {};
}

std::optional<Vector> VectorStore::GetVector(const std::string& id) const {
  std::shared_lock lock(mutex_);

  auto it = vectors_.find(id);
  if (it == vectors_.end()) {
    return std::nullopt;
  }

  return it->second;
}

bool VectorStore::DeleteVector(const std::string& id) {
  std::unique_lock lock(mutex_);

  auto it = vectors_.find(id);
  if (it == vectors_.end()) {
    return false;
  }

  vectors_.erase(it);
  return true;
}

bool VectorStore::HasVector(const std::string& id) const {
  std::shared_lock lock(mutex_);
  return vectors_.find(id) != vectors_.end();
}

std::vector<std::string> VectorStore::GetAllIds() const {
  std::shared_lock lock(mutex_);

  std::vector<std::string> ids;
  ids.reserve(vectors_.size());

  for (const auto& [id, _] : vectors_) {
    ids.push_back(id);
  }

  return ids;
}

size_t VectorStore::GetVectorCount() const {
  std::shared_lock lock(mutex_);
  return vectors_.size();
}

void VectorStore::Clear() {
  std::unique_lock lock(mutex_);
  vectors_.clear();
  dimension_ = 0;  // Reset dimension
}

VectorStoreStatistics VectorStore::GetStatistics() const {
  std::shared_lock lock(mutex_);

  VectorStoreStatistics stats;
  stats.vector_count = vectors_.size();
  stats.dimension = dimension_;
  stats.memory_bytes = MemoryUsage();

  return stats;
}

size_t VectorStore::MemoryUsage() const {
  std::shared_lock lock(mutex_);

  size_t total = 0;

  // Base container overhead
  total += sizeof(*this);

  // Vectors
  for (const auto& [id, vector] : vectors_) {
    // ID string
    total += sizeof(std::string) + id.capacity();

    // Vector data
    total += sizeof(Vector);
    total += vector.data.capacity() * sizeof(float);
  }

  return total;
}

}  // namespace nvecd::vectors
