/**
 * @file snapshot_lock.cpp
 * @brief Synchronous snapshot with global write lock implementation
 */

#include "storage/snapshot_lock.h"

#include "storage/snapshot_format_v1.h"
#include "utils/structured_log.h"

namespace nvecd::storage {

utils::Expected<void, utils::Error> WriteSnapshotWithLock(
    const std::string& filepath, const config::Config& config,
    events::EventStore& event_store, events::CoOccurrenceIndex& co_index,
    vectors::VectorStore& vector_store,
    const SnapshotStatistics* stats,
    const std::unordered_map<std::string, StoreStatistics>*
        store_stats) {
  utils::LogStorageInfo("snapshot_lock",
                        "Acquiring write locks as barrier for consistent snapshot");

  // Write lock barrier: drain all in-flight writes by acquiring exclusive
  // locks on all stores, then release them immediately. This ensures a
  // consistent point-in-time boundary.
  //
  // After this barrier, the caller should have set the read_only flag
  // (via FlagGuard in dump_handler) to prevent new writes from starting.
  // WriteSnapshotV1 then serializes via const getters that acquire
  // shared (read) locks internally.
  {
    auto lock_es = event_store.AcquireWriteLock();
    auto lock_co = co_index.AcquireWriteLock();
    auto lock_vs = vector_store.AcquireWriteLock();
    // All writes drained. Release write locks.
  }

  utils::LogStorageInfo("snapshot_lock",
                        "Write barrier complete, serializing snapshot");

  // Serialize stores — each const getter acquires its own read lock
  auto result = snapshot_v1::WriteSnapshotV1(filepath, config, event_store,
                                             co_index, vector_store, stats,
                                             store_stats);

  // Locks released by RAII when function returns
  if (result) {
    utils::LogStorageInfo("snapshot_lock",
                          "Lock-mode snapshot completed: " + filepath);
  } else {
    utils::LogStorageError("snapshot_lock", filepath,
                           result.error().message());
  }

  return result;
}

}  // namespace nvecd::storage
