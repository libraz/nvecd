/**
 * @file snapshot_lock.h
 * @brief Synchronous snapshot with global write lock
 *
 * Acquires exclusive write locks on all stores during serialization,
 * ensuring point-in-time consistency at the cost of blocking all writes.
 */

#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

#include "config/config.h"
#include "events/co_occurrence_index.h"
#include "events/event_store.h"
#include "storage/snapshot_format.h"
#include "utils/error.h"
#include "utils/expected.h"
#include "vectors/metadata_store.h"
#include "vectors/vector_store.h"

namespace nvecd::storage {

class WriteAheadLog;

/**
 * @brief Write snapshot with global write lock (blocking mode)
 *
 * Acquires exclusive write locks on all three stores simultaneously,
 * then calls WriteSnapshotV1. All write operations are blocked during
 * the snapshot.
 *
 * Lock acquisition order: EventStore -> CoOccurrenceIndex -> VectorStore
 * (consistent ordering to prevent deadlocks)
 *
 * @param filepath Output file path
 * @param config Configuration to serialize
 * @param event_store EventStore (write lock will be acquired)
 * @param co_index CoOccurrenceIndex (write lock will be acquired)
 * @param vector_store VectorStore (write lock will be acquired)
 * @param stats Optional snapshot statistics
 * @param store_stats Optional per-store statistics
 * @param metadata_store Optional metadata store to include in the snapshot
 * @param wal Optional Write-Ahead Log. When provided, its current sequence is
 *   captured WHILE the write-lock barrier is held (so it equals the maximum op
 *   reflected in the snapshot) and written to @p captured_sequence.
 * @param captured_sequence Optional out-param receiving the captured WAL
 *   sequence (set to 0 when @p wal is null). The caller writes the checkpoint
 *   sidecar and truncates the WAL only after this function succeeds.
 * @return Expected<void, Error> Success or error
 */
utils::Expected<void, utils::Error> WriteSnapshotWithLock(
    const std::string& filepath, const config::Config& config, events::EventStore& event_store,
    events::CoOccurrenceIndex& co_index, vectors::VectorStore& vector_store, const SnapshotStatistics* stats = nullptr,
    const std::unordered_map<std::string, StoreStatistics>* store_stats = nullptr,
    vectors::MetadataStore* metadata_store = nullptr, WriteAheadLog* wal = nullptr,
    uint64_t* captured_sequence = nullptr);

}  // namespace nvecd::storage
