/**
 * @file snapshot_fork.h
 * @brief Fork-based COW snapshot writer (Redis BGSAVE style)
 *
 * Uses fork() to create a child process that inherits a frozen copy of
 * all in-memory data via OS copy-on-write. The parent continues serving
 * requests immediately after fork.
 */

#pragma once

#include <sys/types.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

#include "config/config.h"
#include "events/co_occurrence_index.h"
#include "events/event_store.h"
#include "utils/error.h"
#include "utils/expected.h"
#include "vectors/vector_store.h"

namespace nvecd::storage {

/// Status of a background snapshot operation
enum class SnapshotStatus : uint8_t {
  kIdle,        ///< No snapshot in progress
  kInProgress,  ///< Fork child is writing
  kCompleted,   ///< Last snapshot succeeded
  kFailed       ///< Last snapshot failed
};

/// Result of a background snapshot operation
struct SnapshotResult {
  SnapshotStatus status = SnapshotStatus::kIdle;
  std::string filepath;       ///< Path of the snapshot file
  std::string error_message;  ///< Error message if failed
  pid_t child_pid = -1;       ///< PID of fork child (while in progress)
  uint64_t start_time = 0;    ///< When snapshot started (unix timestamp)
  uint64_t end_time = 0;      ///< When snapshot completed (unix timestamp)
};

/**
 * @brief Fork-based COW snapshot writer
 *
 * Manages the lifecycle of background snapshot operations using fork().
 *
 * Thread Safety: Thread-safe (uses mutex for status updates)
 *
 * Usage:
 * @code
 * ForkSnapshotWriter writer;
 * auto result = writer.StartBackgroundSave(path, config, es, co, vs);
 * // ... later ...
 * writer.CheckChild();  // Reap finished child
 * auto status = writer.GetStatus();
 * @endcode
 */
class ForkSnapshotWriter {
 public:
  ForkSnapshotWriter() = default;
  ~ForkSnapshotWriter();

  // Non-copyable, non-movable
  ForkSnapshotWriter(const ForkSnapshotWriter&) = delete;
  ForkSnapshotWriter& operator=(const ForkSnapshotWriter&) = delete;
  ForkSnapshotWriter(ForkSnapshotWriter&&) = delete;
  ForkSnapshotWriter& operator=(ForkSnapshotWriter&&) = delete;

  /**
   * @brief Start a background fork-based snapshot
   *
   * Pre-fork barrier: Acquires exclusive write locks on all stores to
   * ensure no mutex is held at fork time. After fork, parent releases
   * locks immediately.
   *
   * Child process:
   * 1. Closes inherited file descriptors (server sockets, etc.)
   * 2. Shuts down spdlog (avoid writing to parent's log files)
   * 3. Resets signal handlers
   * 4. Calls WriteSnapshotV1 to serialize all stores
   * 5. Calls _exit() (never exit())
   *
   * @param filepath Output file path
   * @param config Configuration to serialize
   * @param event_store EventStore (write lock acquired briefly)
   * @param co_index CoOccurrenceIndex (write lock acquired briefly)
   * @param vector_store VectorStore (write lock acquired briefly)
   * @return Expected<void, Error> Success (fork started) or error
   */
  utils::Expected<void, utils::Error> StartBackgroundSave(
      const std::string& filepath, const config::Config& config,
      events::EventStore& event_store, events::CoOccurrenceIndex& co_index,
      vectors::VectorStore& vector_store);

  /**
   * @brief Check and reap child process (non-blocking)
   *
   * Uses waitpid(WNOHANG) to check if child has exited.
   * Updates status to kCompleted or kFailed accordingly.
   * Safe to call frequently (no-op if no child is running).
   */
  void CheckChild();

  /**
   * @brief Get current snapshot status
   * @return Copy of current SnapshotResult
   */
  SnapshotResult GetStatus() const;

  /**
   * @brief Check if a snapshot is currently in progress
   */
  bool IsInProgress() const;

  /**
   * @brief Wait for child process to finish (blocking)
   *
   * Used during server shutdown. Waits up to timeout_ms for child,
   * then sends SIGTERM if still running.
   *
   * @param timeout_ms Maximum wait time in milliseconds (0 = no wait)
   */
  void WaitForChild(uint32_t timeout_ms = 5000);

 private:
  mutable std::mutex status_mutex_;
  SnapshotResult current_result_;

  /**
   * @brief Child process entry point (runs after fork)
   *
   * This function never returns — it calls _exit().
   */
  [[noreturn]] static void ChildProcess(
      const std::string& filepath, const config::Config& config,
      const events::EventStore& event_store,
      const events::CoOccurrenceIndex& co_index,
      const vectors::VectorStore& vector_store);

  /**
   * @brief Close all file descriptors >= min_fd
   */
  static void CloseInheritedFDs(int min_fd);
};

}  // namespace nvecd::storage
