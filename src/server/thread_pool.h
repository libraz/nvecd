/**
 * @file thread_pool.h
 * @brief Thread pool for handling concurrent client connections
 */

#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace nvecd::server {

/**
 * @brief Thread pool for executing tasks concurrently
 *
 * Features:
 * - Fixed number of worker threads
 * - Bounded task queue with backpressure
 * - Graceful shutdown
 * - Thread-safe task submission
 */
class ThreadPool {
 public:
  using Task = std::function<void()>;

  /**
   * @brief Construct thread pool
   * @param num_threads Number of worker threads (0 = CPU count)
   * @param queue_size Maximum queue size (0 = unbounded)
   */
  explicit ThreadPool(size_t num_threads = 0, size_t queue_size = 0);

  /**
   * @brief Destructor - waits for all tasks to complete
   */
  ~ThreadPool();

  // Non-copyable and non-movable
  ThreadPool(const ThreadPool&) = delete;
  ThreadPool& operator=(const ThreadPool&) = delete;
  ThreadPool(ThreadPool&&) = delete;
  ThreadPool& operator=(ThreadPool&&) = delete;

  /**
   * @brief Submit task to pool
   * @param task Task to execute
   * @return true if submitted, false if queue is full
   */
  bool Submit(Task task);

  /**
   * @brief Get number of worker threads
   */
  size_t GetThreadCount() const { return workers_.size(); }

  /**
   * @brief Get number of pending tasks
   */
  size_t GetQueueSize() const;

  /**
   * @brief Check if pool is shutting down
   */
  bool IsShutdown() const;

  /**
   * @brief Shutdown pool and wait for running tasks up to the requested limit
   * @param graceful If true, run pending tasks before stopping. If false, abandon pending tasks.
   * @param timeout_ms Maximum time to wait for pending and running tasks (0 = no timeout).
   *        On timeout, pending tasks are discarded and workers are detached.
   *        Running tasks are not force-cancelled; they retain only shared worker
   *        state and can finish safely after this object is destroyed.
   */
  void Shutdown(bool graceful = true, uint32_t timeout_ms = 0);

 private:
  struct State {
    std::queue<Task> tasks;
    mutable std::mutex queue_mutex;
    std::condition_variable condition;
    std::condition_variable idle_condition;
    std::atomic<bool> shutdown{false};
    std::atomic<size_t> active_workers{0};
    size_t max_queue_size = 0;
  };

  std::vector<std::thread> workers_;
  std::shared_ptr<State> state_;
  std::mutex shutdown_mutex_;

  /**
   * @brief Worker thread function
   */
  static void WorkerThread(std::shared_ptr<State> state);
};

}  // namespace nvecd::server
