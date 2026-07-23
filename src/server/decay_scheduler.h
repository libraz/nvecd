/**
 * @file decay_scheduler.h
 * @brief Background co-occurrence decay/prune scheduler
 *
 * Periodically applies exponential decay to the co-occurrence index and prunes
 * negligible entries, mirroring the structure of SnapshotScheduler.
 */

#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <thread>

#include "events/co_occurrence_index.h"
#include "utils/error.h"
#include "utils/expected.h"

namespace nvecd::server {

/**
 * @brief Background co-occurrence decay scheduler
 *
 * Every @c interval_sec seconds, multiplies all co-occurrence scores by
 * @c alpha (favoring recent co-occurrences) and periodically prunes entries
 * that fall below the index's configured thresholds. Without this scheduler the
 * co-occurrence scores grow without bound and the "recent co-occurrence
 * preferred" behavior never takes effect.
 *
 * Thread Safety:
 * - Start/Stop are not thread-safe (call from main thread only)
 * - The underlying CoOccurrenceIndex operations are internally synchronized
 */
class DecayScheduler {
 public:
  using MaintenanceCallback = std::function<utils::Expected<void, utils::Error>(double alpha, bool prune)>;
  /**
   * @brief Construct a DecayScheduler
   * @param co_index Co-occurrence index to decay (non-owning, must outlive this)
   * @param interval_sec Decay interval in seconds (<= 0 disables the scheduler)
   * @param alpha Decay factor passed to ApplyDecay (0.0 < alpha <= 1.0)
   */
  DecayScheduler(events::CoOccurrenceIndex* co_index, int interval_sec, double alpha,
                 MaintenanceCallback maintenance_callback = {});

  // Non-copyable and non-movable
  DecayScheduler(const DecayScheduler&) = delete;
  DecayScheduler& operator=(const DecayScheduler&) = delete;
  DecayScheduler(DecayScheduler&&) = delete;
  DecayScheduler& operator=(DecayScheduler&&) = delete;

  ~DecayScheduler();

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
   * Calculates the next decay time, sleeps in 1-second intervals checking for
   * shutdown, and triggers ApplyDecay (and periodically Prune) when time
   * expires.
   */
  void SchedulerLoop();

  events::CoOccurrenceIndex* co_index_;
  int interval_sec_;
  double alpha_;
  MaintenanceCallback maintenance_callback_;

  std::atomic<bool> running_{false};
  std::unique_ptr<std::thread> scheduler_thread_;
};

}  // namespace nvecd::server
