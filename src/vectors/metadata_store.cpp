/**
 * @file metadata_store.cpp
 * @brief Metadata store implementation
 */

#include "vectors/metadata_store.h"

namespace nvecd::vectors {

void MetadataStore::Set(const std::string& id, Metadata meta) {
  std::unique_lock lock(mutex_);
  // Presence is determined solely by map membership. An explicitly set but
  // empty metadata map is retained so that it reports as present (with an
  // empty value), distinct from an item that was never set. Callers wanting
  // to remove an entry must use Delete().
  metadata_[id] = std::move(meta);
}

const Metadata* MetadataStore::Get(const std::string& id) const {
  std::shared_lock lock(mutex_);
  auto it = metadata_.find(id);
  if (it == metadata_.end()) {
    return nullptr;
  }
  return &it->second;
}

void MetadataStore::Delete(const std::string& id) {
  std::unique_lock lock(mutex_);
  metadata_.erase(id);
}

std::vector<std::string> MetadataStore::Filter(const MetadataFilter& filter,
                                               const std::vector<std::string>& candidates) const {
  std::shared_lock lock(mutex_);
  std::vector<std::string> result;

  if (filter.Empty()) {
    // No filter: return all candidates or all items with metadata.
    if (!candidates.empty()) {
      return candidates;
    }
    result.reserve(metadata_.size());
    for (const auto& entry : metadata_) {
      result.push_back(entry.first);
    }
    return result;
  }

  if (!candidates.empty()) {
    // Filter only the given candidates.
    result.reserve(candidates.size());
    for (const auto& id : candidates) {
      auto it = metadata_.find(id);
      if (it != metadata_.end() && filter.Match(it->second)) {
        result.push_back(id);
      }
    }
  } else {
    // Linear scan all items.
    result.reserve(metadata_.size() / 4);  // Heuristic
    for (const auto& [id, meta] : metadata_) {
      if (filter.Match(meta)) {
        result.push_back(id);
      }
    }
  }

  return result;
}

bool MetadataStore::Matches(const std::string& id, const MetadataFilter& filter) const {
  std::shared_lock lock(mutex_);
  if (filter.Empty()) {
    return true;
  }
  auto it = metadata_.find(id);
  if (it == metadata_.end()) {
    return false;
  }
  return filter.Match(it->second);
}

uint32_t MetadataStore::Size() const {
  std::shared_lock lock(mutex_);
  return static_cast<uint32_t>(metadata_.size());
}

void MetadataStore::Clear() {
  std::unique_lock lock(mutex_);
  metadata_.clear();
}

void MetadataStore::SwapState(MetadataStore& other) {
  if (this == &other) {
    return;
  }
  std::scoped_lock lock(mutex_, other.mutex_);
  metadata_.swap(other.metadata_);
}

std::shared_lock<std::shared_mutex> MetadataStore::AcquireReadLock() const {
  return std::shared_lock(mutex_);
}

std::unique_lock<std::shared_mutex> MetadataStore::AcquireWriteLock() {
  return std::unique_lock(mutex_);
}

}  // namespace nvecd::vectors
