/**
 * @file periodic_worker.h
 * @brief Background thread that runs a callback at a fixed period
 */

#pragma once

#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>

namespace nvecd::utils {

/**
 * @brief Runs a tick callback every period on its own thread.
 *
 * The first tick fires one period after Start(). Stop() wakes the thread
 * through a condition variable, so it returns as soon as a running tick
 * finishes rather than after the remaining period.
 *
 * Start/Stop are not thread-safe against each other, and Stop() must not be
 * called from inside the tick.
 */
class PeriodicWorker {
 public:
  using Tick = std::function<void()>;

  PeriodicWorker() = default;
  ~PeriodicWorker() { Stop(); }

  PeriodicWorker(const PeriodicWorker&) = delete;
  PeriodicWorker& operator=(const PeriodicWorker&) = delete;
  PeriodicWorker(PeriodicWorker&&) = delete;
  PeriodicWorker& operator=(PeriodicWorker&&) = delete;

  /// Start the thread; a no-op while already started.
  void Start(std::chrono::milliseconds period, Tick tick) {
    if (thread_.joinable()) {
      return;
    }
    stop_requested_ = false;
    thread_ = std::thread([this, period, tick = std::move(tick)]() { Run(period, tick); });
  }

  /// Wake the thread and join it; a no-op when not started.
  void Stop() {
    {
      std::lock_guard lock(mutex_);
      stop_requested_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) {
      thread_.join();
    }
  }

 private:
  void Run(std::chrono::milliseconds period, const Tick& tick) {
    std::unique_lock lock(mutex_);
    while (!cv_.wait_for(lock, period, [this]() { return stop_requested_; })) {
      lock.unlock();
      tick();
      lock.lock();
    }
  }

  std::mutex mutex_;
  std::condition_variable cv_;
  bool stop_requested_ = false;
  std::thread thread_;
};

}  // namespace nvecd::utils
