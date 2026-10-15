/**
 * @file snapshot_scheduler.h
 * @brief Background snapshot scheduler for periodic auto-snapshots
 *
 * Reference: ../mygram-db/src/server/snapshot_scheduler.h
 * Reusability: 80% (removed MySQL/GTID/TableCatalog, adapted for nvecd stores)
 */

#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <string>

#include "config/config.h"
#include "server/server_types.h"
#include "utils/periodic_worker.h"

namespace nvecd::server {

/**
 * @brief Remove snapshot temporaries whose writing process no longer exists
 *
 * A temporary is named ".<snapshot>.tmp.<pid>.<n>" and can only be published by
 * the process that created it, so one left behind by a process that is gone can
 * never complete. Destructors reclaim temporaries on every orderly exit; a
 * parent that is killed outright runs none, which makes the startup call the
 * only path that recovers its files. Safe to call when @p dir does not exist.
 *
 * @param dir Snapshot directory to scan
 */
void ReclaimAbandonedTemporaries(const std::string& dir);

/**
 * @brief Background snapshot scheduler
 *
 * Periodically creates fork-based snapshots and cleans up old files
 * based on the configured retention policy.
 *
 * Key responsibilities:
 * - Periodic snapshot creation via ForkSnapshotWriter
 * - Cleanup of old auto-snapshot files
 * - Mutual exclusion with manual DUMP SAVE and DUMP LOAD through the shared gates
 *
 * Thread Safety:
 * - Start/Stop are not thread-safe (call from main thread only)
 * - Internal operations are thread-safe
 */
class SnapshotScheduler {
 public:
  /**
   * @brief Construct a SnapshotScheduler
   * @param config Snapshot configuration (interval, retain, dir)
   * @param ctx Handler context whose stores, fork writer and write gates every
   *            snapshot is taken through (non-owning, must outlive the scheduler)
   */
  SnapshotScheduler(config::SnapshotConfig config, HandlerContext& ctx);

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
   * Sets running_ to false, wakes and joins the scheduler thread, then drains
   * a finished snapshot session.
   */
  void Stop();

  /**
   * @brief Check if scheduler is running
   * @return true if the scheduler thread is active
   */
  bool IsRunning() const { return running_; }

 private:
  /**
   * @brief One scheduler tick (runs on the worker thread every second)
   *
   * Publishes a finished snapshot session, then takes a snapshot when the
   * interval has elapsed.
   */
  void Tick();

  /**
   * @brief Publish the outcome of a finished fork session and clean up after it
   */
  void CheckFinishedSnapshot();

  /**
   * @brief Take a snapshot using ForkSnapshotWriter
   *
   * Generates a timestamped filename and starts a background fork save through
   * the same gated capture routine as DUMP SAVE.
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
  HandlerContext& ctx_;

  std::atomic<bool> running_{false};
  std::chrono::steady_clock::time_point next_save_time_;  // Worker thread only
  utils::PeriodicWorker worker_;
};

}  // namespace nvecd::server
