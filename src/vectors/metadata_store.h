/**
 * @file metadata_store.h
 * @brief Per-item metadata storage with filter support
 *
 * Stores metadata for each item keyed by the stable external string item ID.
 * Keying by ID (rather than by VectorStore compact_index) keeps metadata
 * correctly associated with its item across defragmentation and tombstone-slot
 * reuse, which both change the compact_index of an item without notice.
 *
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
#include <string>
#include <unordered_map>
#include <vector>

#include "vectors/metadata.h"
#include "vectors/metadata_filter.h"

namespace nvecd::vectors {

/**
 * @brief Thread-safe metadata store keyed by stable item ID
 */
class MetadataStore {
 public:
  MetadataStore() = default;

  /**
   * @brief Set metadata for an item
   * @param id Item's stable external ID
   * @param meta Metadata key-value pairs
   */
  void Set(const std::string& id, Metadata meta);

  /**
   * @brief Get metadata for an item
   * @param id Item's stable external ID
   * @return Pointer to metadata (null if not found), valid while holding read lock
   */
  const Metadata* Get(const std::string& id) const;

  /**
   * @brief Delete metadata for an item
   * @param id Item's stable external ID
   */
  void Delete(const std::string& id);

  /**
   * @brief Filter items by metadata conditions (linear scan)
   * @param filter AND-combined filter conditions
   * @param candidates If non-empty, only check these item IDs
   * @return Item IDs that match all conditions
   */
  std::vector<std::string> Filter(const MetadataFilter& filter, const std::vector<std::string>& candidates = {}) const;

  /**
   * @brief Check if a single item matches the filter
   * @param id Item to check
   * @param filter Filter conditions
   * @return True if matches (or filter is empty)
   */
  bool Matches(const std::string& id, const MetadataFilter& filter) const;

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
   * @brief Get all metadata as (id -> metadata) pairs (for serialization).
   *        Caller must hold read lock.
   */
  const std::unordered_map<std::string, Metadata>& GetAll() const { return metadata_; }

 private:
  mutable std::shared_mutex mutex_;
  std::unordered_map<std::string, Metadata> metadata_;  ///< Keyed by item ID
};

}  // namespace nvecd::vectors
