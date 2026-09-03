/**
 * @file thread_pool.cpp
 * @brief Thread pool implementation
 */

#include "thread_pool.h"

#include <spdlog/spdlog.h>

#include <chrono>

#include "utils/structured_log.h"

namespace nvecd::server {

ThreadPool::ThreadPool(size_t num_threads, size_t queue_size) : state_(std::make_shared<State>()) {
  state_->max_queue_size = queue_size;
  // Default to CPU count if not specified
  if (num_threads == 0) {
    num_threads = std::thread::hardware_concurrency();
    if (num_threads == 0) {
      num_threads = 4;  // Fallback
    }
  }

  spdlog::info("Creating thread pool with {} workers, queue size: {}", num_threads,
               queue_size == 0 ? "unbounded" : std::to_string(queue_size));

  // Start worker threads
  workers_.reserve(num_threads);
  for (size_t i = 0; i < num_threads; ++i) {
    workers_.emplace_back(&ThreadPool::WorkerThread, state_);
  }
}

ThreadPool::~ThreadPool() {
  Shutdown();
}

bool ThreadPool::Submit(Task task) {
  {
    std::unique_lock<std::mutex> lock(state_->queue_mutex);

    // Check if shutting down
    if (state_->shutdown) {
      return false;
    }

    // Check queue size limit
    if (state_->max_queue_size > 0 && state_->tasks.size() >= state_->max_queue_size) {
      return false;  // Queue is full
    }

    // Add task to queue
    state_->tasks.push(std::move(task));
  }

  // Notify one worker
  state_->condition.notify_one();
  return true;
}

size_t ThreadPool::GetQueueSize() const {
  std::scoped_lock lock(state_->queue_mutex);
  return state_->tasks.size();
}

bool ThreadPool::IsShutdown() const {
  return state_->shutdown.load();
}

void ThreadPool::Shutdown(uint32_t timeout_ms) {
  std::scoped_lock lifecycle_lock(shutdown_mutex_);

  {
    // Publish under the queue lock so a worker that just evaluated its wait
    // predicate cannot miss the notification below.
    std::scoped_lock lock(state_->queue_mutex);
    state_->shutdown.store(true);
  }

  // Wake up all workers
  state_->condition.notify_all();

  if (timeout_ms > 0) {
    std::unique_lock<std::mutex> lock(state_->queue_mutex);
    const bool drained = state_->idle_condition.wait_for(lock, std::chrono::milliseconds(timeout_ms), [this] {
      return state_->tasks.empty() && state_->active_workers.load() == 0;
    });
    if (!drained) {
      // Discard work that has not started so the join below cannot be extended
      // indefinitely by the backlog. A task already running is still waited
      // for: it may reference state the caller destroys once Shutdown returns.
      const size_t queued = state_->tasks.size();
      while (!state_->tasks.empty()) {
        state_->tasks.pop();
      }
      lock.unlock();
      state_->condition.notify_all();

      nvecd::utils::StructuredLog()
          .Event("server_warning")
          .Field("operation", "thread_pool_shutdown")
          .Field("type", "timeout_reached")
          .Field("discarded_tasks", static_cast<uint64_t>(queued))
          .Field("active_workers", static_cast<uint64_t>(state_->active_workers.load()))
          .Warn();
    }
  }

  // Joining is the only exit. Detaching would let a worker keep running against
  // state the caller frees immediately after this returns.
  for (auto& worker : workers_) {
    if (worker.joinable()) {
      worker.join();
    }
  }
  spdlog::info("Thread pool shut down");
}

void ThreadPool::WorkerThread(std::shared_ptr<State> state) {
  while (true) {
    Task task;

    {
      std::unique_lock<std::mutex> lock(state->queue_mutex);

      // Wait for task or shutdown
      state->condition.wait(lock, [&state] { return state->shutdown || !state->tasks.empty(); });

      // Exit if shutting down and no more tasks
      if (state->shutdown && state->tasks.empty()) {
        return;
      }

      // Get next task
      // Note: This check is technically redundant after the wait() condition,
      // but kept for defensive programming and clarity
      if (!state->tasks.empty()) {
        state->active_workers++;  // Increment while holding lock to prevent premature shutdown detection
        task = std::move(state->tasks.front());
        state->tasks.pop();
      }
    }

    // Execute task (outside lock)
    if (task) {
      try {
        task();
      } catch (const std::exception& e) {
        nvecd::utils::StructuredLog()
            .Event("server_error")
            .Field("type", "worker_thread_exception")
            .Field("error", e.what())
            .Error();
      } catch (...) {
        nvecd::utils::StructuredLog().Event("server_error").Field("type", "worker_thread_unknown_exception").Error();
      }
      state->active_workers--;  // Decrement after task completes
      state->idle_condition.notify_all();
    }
  }
}

}  // namespace nvecd::server
