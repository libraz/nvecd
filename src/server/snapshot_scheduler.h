/**
 * @file snapshot_scheduler.h
 * @brief Background snapshot scheduler for periodic auto-snapshots
 *
 * Reference: ../mygram-db/src/server/snapshot_scheduler.h
 * Reusability: 80% (removed MySQL/GTID/TableCatalog, adapted for nvecd stores)
 */

#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <thread>

#include "config/config.h"
#include "events/co_occurrence_index.h"
#include "events/event_store.h"
#include "storage/snapshot_fork.h"
#include "vectors/vector_store.h"

namespace nvecd::server {

/**
 * @brief Background snapshot scheduler
 *
 * Periodically creates fork-based snapshots and cleans up old files
 * based on the configured retention policy.
 *
 * Key responsibilities:
 * - Periodic snapshot creation via ForkSnapshotWriter
 * - Cleanup of old auto-snapshot files
 * - Mutual exclusion with manual DUMP SAVE operations
 *
 * Thread Safety:
 * - Start/Stop are not thread-safe (call from main thread only)
 * - Internal operations are thread-safe
 */
class SnapshotScheduler {
 public:
  /**
   * @brief Construct a SnapshotScheduler
   * @param config Snapshot configuration (interval, retain, dir, mode)
   * @param fork_writer Fork-based snapshot writer (non-owning)
   * @param full_config Full configuration for snapshot metadata (non-owning)
   * @param event_store Event store for snapshot data (non-owning)
   * @param co_index Co-occurrence index for snapshot data (non-owning)
   * @param vector_store Vector store for snapshot data (non-owning)
   * @param read_only Reference to read_only flag for mutual exclusion with manual DUMP SAVE
   */
  SnapshotScheduler(config::SnapshotConfig config, storage::ForkSnapshotWriter* fork_writer,
                    const config::Config* full_config, events::EventStore* event_store,
                    events::CoOccurrenceIndex* co_index, vectors::VectorStore* vector_store,
                    std::atomic<bool>& read_only);

  // Non-copyable and non-movable
  SnapshotScheduler(const SnapshotScheduler&) = delete;
  SnapshotScheduler& operator=(const SnapshotScheduler&) = delete;
  SnapshotScheduler(SnapshotScheduler&&) = delete;
  SnapshotScheduler& operator=(SnapshotScheduler&&) = delete;

  ~SnapshotScheduler();

  /**
   * @brief Start the scheduler
   *
   * If interval_sec <= 0, logs "disabled" and returns without starting.
   * Otherwise, launches the background scheduler thread.
   */
  void Start();

  /**
   * @brief Stop the scheduler
   *
   * Sets running_ to false and joins the scheduler thread.
   */
  void Stop();

  /**
   * @brief Check if scheduler is running
   * @return true if the scheduler thread is active
   */
  bool IsRunning() const { return running_; }

 private:
  /**
   * @brief Scheduler loop (runs in background thread)
   *
   * Calculates next save time, sleeps in 1-second intervals checking
   * for shutdown, and triggers snapshot + cleanup when time expires.
   */
  void SchedulerLoop();

  /**
   * @brief Take a snapshot using ForkSnapshotWriter
   *
   * Acquires exclusive access via compare_exchange_strong on read_only_,
   * generates a timestamped filename, and starts a background fork save.
   */
  void TakeSnapshot();

  /**
   * @brief Clean up old auto-snapshots based on retention policy
   *
   * Iterates the snapshot directory, collects files matching auto_*.nvec,
   * sorts by modification time (newest first), and deletes files beyond
   * the retain count.
   */
  void CleanupOldSnapshots();

  config::SnapshotConfig config_;
  storage::ForkSnapshotWriter* fork_writer_;
  const config::Config* full_config_;
  events::EventStore* event_store_;
  events::CoOccurrenceIndex* co_index_;
  vectors::VectorStore* vector_store_;
  std::atomic<bool>& read_only_;

  std::atomic<bool> running_{false};
  std::unique_ptr<std::thread> scheduler_thread_;
};

}  // namespace nvecd::server
