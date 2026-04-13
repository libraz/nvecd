/**
 * @file metadata_store.cpp
 * @brief Metadata store implementation
 */

#include "vectors/metadata_store.h"

namespace nvecd::vectors {

void MetadataStore::Set(uint32_t compact_index, Metadata meta) {
  std::unique_lock lock(mutex_);
  if (compact_index >= metadata_.size()) {
    metadata_.resize(compact_index + 1);
  }
  metadata_[compact_index] = std::move(meta);
}

const Metadata* MetadataStore::Get(uint32_t compact_index) const {
  std::shared_lock lock(mutex_);
  if (compact_index >= metadata_.size()) {
    return nullptr;
  }
  if (metadata_[compact_index].empty()) {
    return nullptr;
  }
  return &metadata_[compact_index];
}

void MetadataStore::Delete(uint32_t compact_index) {
  std::unique_lock lock(mutex_);
  if (compact_index < metadata_.size()) {
    metadata_[compact_index].clear();
  }
}

std::vector<uint32_t> MetadataStore::Filter(
    const MetadataFilter& filter,
    const std::vector<uint32_t>& candidates) const {
  std::shared_lock lock(mutex_);
  std::vector<uint32_t> result;

  if (filter.Empty()) {
    // No filter: return all candidates or all items
    if (!candidates.empty()) {
      return candidates;
    }
    result.reserve(metadata_.size());
    for (uint32_t i = 0; i < metadata_.size(); ++i) {
      if (!metadata_[i].empty()) {
        result.push_back(i);
      }
    }
    return result;
  }

  if (!candidates.empty()) {
    // Filter only candidates
    result.reserve(candidates.size());
    for (uint32_t idx : candidates) {
      if (idx < metadata_.size() && filter.Match(metadata_[idx])) {
        result.push_back(idx);
      }
    }
  } else {
    // Linear scan all items
    result.reserve(metadata_.size() / 4);  // Heuristic
    for (uint32_t i = 0; i < metadata_.size(); ++i) {
      if (!metadata_[i].empty() && filter.Match(metadata_[i])) {
        result.push_back(i);
      }
    }
  }

  return result;
}

bool MetadataStore::Matches(uint32_t compact_index,
                             const MetadataFilter& filter) const {
  std::shared_lock lock(mutex_);
  if (filter.Empty()) {
    return true;
  }
  if (compact_index >= metadata_.size()) {
    return false;
  }
  return filter.Match(metadata_[compact_index]);
}

uint32_t MetadataStore::Size() const {
  std::shared_lock lock(mutex_);
  uint32_t count = 0;
  for (const auto& m : metadata_) {
    if (!m.empty()) {
      count++;
    }
  }
  return count;
}

void MetadataStore::Clear() {
  std::unique_lock lock(mutex_);
  metadata_.clear();
}

std::shared_lock<std::shared_mutex> MetadataStore::AcquireReadLock() const {
  return std::shared_lock(mutex_);
}

std::unique_lock<std::shared_mutex> MetadataStore::AcquireWriteLock() {
  return std::unique_lock(mutex_);
}

}  // namespace nvecd::vectors
