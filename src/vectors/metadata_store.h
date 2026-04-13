/**
 * @file metadata_store.h
 * @brief Per-item metadata storage with filter support
 *
 * Stores metadata for each item (keyed by compact_index from VectorStore).
 * Provides linear-scan filtering (Phase 1) with planned inverted index
 * support (Phase 2).
 *
 * Thread-safety:
 * - Multiple concurrent reads (Get, Filter) via shared_mutex
 * - Exclusive writes (Set, Delete)
 */

#pragma once

#include <cstdint>
#include <mutex>
#include <shared_mutex>
#include <vector>

#include "vectors/metadata.h"
#include "vectors/metadata_filter.h"

namespace nvecd::vectors {

/**
 * @brief Thread-safe metadata store indexed by compact_index
 */
class MetadataStore {
 public:
  MetadataStore() = default;

  /**
   * @brief Set metadata for an item
   * @param compact_index Item's compact index in VectorStore
   * @param meta Metadata key-value pairs
   */
  void Set(uint32_t compact_index, Metadata meta);

  /**
   * @brief Get metadata for an item
   * @param compact_index Item's compact index
   * @return Pointer to metadata (null if not found), valid while holding read lock
   */
  const Metadata* Get(uint32_t compact_index) const;

  /**
   * @brief Delete metadata for an item
   * @param compact_index Item's compact index
   */
  void Delete(uint32_t compact_index);

  /**
   * @brief Filter items by metadata conditions (linear scan)
   * @param filter AND-combined filter conditions
   * @param candidates If non-empty, only check these compact_indices
   * @return Compact indices that match all conditions
   */
  std::vector<uint32_t> Filter(const MetadataFilter& filter, const std::vector<uint32_t>& candidates = {}) const;

  /**
   * @brief Check if a single item matches the filter
   * @param compact_index Item to check
   * @param filter Filter conditions
   * @return True if matches (or filter is empty)
   */
  bool Matches(uint32_t compact_index, const MetadataFilter& filter) const;

  /**
   * @brief Get number of items with metadata
   */
  uint32_t Size() const;

  /**
   * @brief Clear all metadata
   */
  void Clear();

  /**
   * @brief Acquire read lock for batch operations
   */
  std::shared_lock<std::shared_mutex> AcquireReadLock() const;

  /**
   * @brief Acquire write lock for snapshot consistency
   */
  std::unique_lock<std::shared_mutex> AcquireWriteLock();

  /**
   * @brief Get all metadata (for serialization). Caller must hold read lock.
   */
  const std::vector<Metadata>& GetAll() const { return metadata_; }

 private:
  mutable std::shared_mutex mutex_;
  std::vector<Metadata> metadata_;  ///< Indexed by compact_index
};

}  // namespace nvecd::vectors
