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
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>

#include "config/config.h"
#include "events/co_occurrence_index.h"
#include "events/event_store.h"
#include "storage/snapshot_session.h"
#include "utils/error.h"
#include "utils/expected.h"
#include "vectors/metadata_store.h"
#include "vectors/vector_store.h"

namespace nvecd::storage {

class WriteAheadLog;

/// Default grace period given to a snapshot child before it is signalled.
constexpr uint32_t kDefaultChildWaitMs = 5000;

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
  uint64_t wal_sequence = 0;  ///< WAL sequence captured under the pre-fork barrier (0 = WAL disabled)
};

/**
 * @brief Fork-based COW snapshot writer
 *
 * Manages the lifecycle of background snapshot operations using fork().
 *
 * The writer owns the fork and the reported status; the durability handshake
 * that follows the fork belongs to SnapshotSession, which completes it on its
 * own and hands back a terminal outcome. CheckChild() therefore only publishes
 * an outcome that already exists — it never reaps a child itself, so no two
 * callers can race over one child's exit status.
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

  /**
   * @brief Wire the Write-Ahead Log for post-snapshot checkpoint and truncation
   *
   * When set, StartBackgroundSave() captures the WAL's current sequence under
   * the pre-fork store-lock barrier (so it equals the maximum op included in the
   * frozen snapshot), and CheckChild() writes the checkpoint sidecar and
   * truncates the WAL up to that sequence once the child completes successfully.
   *
   * @param wal Write-Ahead Log (non-owning, may be null to disable)
   */
  void SetWal(WriteAheadLog* wal) { wal_ = wal; }

  // Non-copyable, non-movable
  ForkSnapshotWriter(const ForkSnapshotWriter&) = delete;
  ForkSnapshotWriter& operator=(const ForkSnapshotWriter&) = delete;
  ForkSnapshotWriter(ForkSnapshotWriter&&) = delete;
  ForkSnapshotWriter& operator=(ForkSnapshotWriter&&) = delete;

  /**
   * @brief Start a background fork-based snapshot
   *
   * Pre-fork barrier: Acquires shared locks on all stores simultaneously to
   * drain and exclude writers. After fork, parent releases locks immediately.
   *
   * Child process (post-fork path is async-signal-safe with respect to
   * application locks; it never re-enters spdlog, whose registry/sink mutex a
   * sibling thread could have held at fork time):
   * 1. Closes inherited file descriptors (server sockets, parent log sinks)
   * 2. Resets signal handlers
   * 3. Calls WriteSnapshotV1 (with logging suppressed) to serialize all stores
   * 4. Reports failure via an async-signal-safe write(2), then _exit()
   *
   * @param filepath Output file path
   * @param config Configuration to serialize
   * @param event_store EventStore (shared lock acquired briefly)
   * @param co_index CoOccurrenceIndex (shared lock acquired briefly)
   * @param vector_store VectorStore (shared lock acquired briefly)
   * @return Expected<void, Error> Success (fork started) or error
   */
  utils::Expected<void, utils::Error> StartBackgroundSave(const std::string& filepath, const config::Config& config,
                                                          events::EventStore& event_store,
                                                          events::CoOccurrenceIndex& co_index,
                                                          vectors::VectorStore& vector_store,
                                                          vectors::MetadataStore* metadata_store = nullptr);

  /**
   * @brief Publish a finished snapshot's terminal status (non-blocking)
   *
   * Returns immediately while the active session's writer is still running.
   * Once the session has completed its durability handshake, the reported
   * status becomes kCompleted or kFailed and the session is released so the
   * next snapshot can start. Safe to call frequently and from several threads:
   * publishing the same terminal outcome twice is a no-op.
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
   * @brief Wait for the active snapshot to reach a terminal state (blocking)
   *
   * Used during server shutdown. Gives the child @p timeout_ms to exit on its
   * own, then signals it and reaps it. Returns only once the durability
   * handshake has completed and its result has been published.
   *
   * @param timeout_ms Grace period for a voluntary child exit, in milliseconds
   */
  void WaitForChild(uint32_t timeout_ms = kDefaultChildWaitMs);

 private:
  /// Publish @p outcome as the terminal status of @p session, exactly once.
  void PublishOutcome(const std::shared_ptr<SnapshotSession>& session,
                      const utils::Expected<SnapshotOutcome, utils::Error>& outcome);

  mutable std::mutex status_mutex_;
  SnapshotResult current_result_;

  /// Durability handshake of the snapshot currently in flight (null when idle).
  std::shared_ptr<SnapshotSession> session_;

  /// Write-Ahead Log for checkpoint/truncate (non-owning, may be null).
  WriteAheadLog* wal_ = nullptr;

  /**
   * @brief The store read locks a forked child inherits from the pre-fork barrier
   *
   * These are the only synchronisation objects the child can block on: each
   * store owns exactly one shared_mutex, and the serializer reaches every store
   * through const getters that take it. Handing the whole set to ChildProcess
   * by value is what makes their re-initialization unforgettable — the child
   * entry point cannot be called without surrendering them, and re-initializing
   * them is the first thing it does.
   */
  struct InheritedStoreLocks {
    std::shared_lock<std::shared_mutex> event_store;
    std::shared_lock<std::shared_mutex> co_index;
    std::shared_lock<std::shared_mutex> vector_store;
    std::shared_lock<std::shared_mutex> metadata_store;
  };

  /**
   * @brief Child process entry point (runs after fork)
   *
   * Restricted to operations that do not depend on a lock a sibling thread may
   * have held at fork time. In particular it must not call into spdlog; all
   * diagnostics use async-signal-safe write(2). This function never returns —
   * it calls _exit().
   *
   * @param locks Inherited store locks; re-initialized before any store access
   */
  [[noreturn]] static void ChildProcess(InheritedStoreLocks locks, const std::string& filepath,
                                        const config::Config& config, const events::EventStore& event_store,
                                        const events::CoOccurrenceIndex& co_index,
                                        const vectors::VectorStore& vector_store,
                                        const vectors::MetadataStore* metadata_store);

  /**
   * @brief Close all file descriptors >= min_fd
   */
  static void CloseInheritedFDs(int min_fd);
};

}  // namespace nvecd::storage
