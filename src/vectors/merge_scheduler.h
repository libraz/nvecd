/**
 * @file merge_scheduler.h
 * @brief Background scheduler for delta-to-main merge and main rebuild
 *
 * Periodically checks whether the TieredVectorStore needs a merge
 * (delta exceeds threshold) or a rebuild (tombstone ratio too high)
 * and triggers the appropriate operation.
 *
 * Thread-safety: Start/Stop are not thread-safe; call from a single thread.
 */

#pragma once

#include <atomic>
#include <chrono>

#include "utils/periodic_worker.h"

namespace nvecd::vectors {

class TieredVectorStore;

/**
 * @brief Background merge/rebuild scheduler for TieredVectorStore
 */
class MergeScheduler {
 public:
  struct Config {
    std::chrono::seconds check_interval{60};  ///< How often to check thresholds
  };

  MergeScheduler();
  explicit MergeScheduler(const Config& config);
  ~MergeScheduler();

  MergeScheduler(const MergeScheduler&) = delete;
  MergeScheduler& operator=(const MergeScheduler&) = delete;

  /**
   * @brief Start the background scheduler
   * @param store TieredVectorStore to monitor (must outlive scheduler)
   */
  void Start(TieredVectorStore* store);

  /**
   * @brief Stop the scheduler and join the worker thread
   */
  void Stop();

  /**
   * @brief Check if the scheduler is running
   */
  bool IsRunning() const { return running_.load(std::memory_order_acquire); }

 private:
  void CheckAndMaintain();

  Config config_;
  TieredVectorStore* store_ = nullptr;
  utils::PeriodicWorker worker_;
  std::atomic<bool> running_{false};
};

}  // namespace nvecd::vectors
