/**
 * @file snapshot_session.h
 * @brief Single owner of the snapshot durability handshake
 *
 * Writing a snapshot is only half of the operation. The other half — collect
 * the writer's terminal result, record the WAL checkpoint sidecar, truncate the
 * WAL, reclaim anything the writer left behind — is one indivisible procedure,
 * and it is identical for both snapshot modes. SnapshotSession owns it.
 */

#pragma once

#include <sys/types.h>

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "utils/error.h"
#include "utils/expected.h"

namespace nvecd::storage {

class WriteAheadLog;

/// Exit code with which a snapshot child reports a fully written snapshot.
constexpr int kSnapshotChildExitSuccess = 0;

/// Exit code with which a snapshot child reports a failed write.
constexpr int kSnapshotChildExitFailure = 1;

/// Side effects produced by a snapshot session that ran to completion.
struct SnapshotOutcome {
  std::string filepath;                 ///< Snapshot that was published
  uint64_t wal_sequence = 0;            ///< Sequence captured under the pre-write barrier
  bool wal_checkpoint_written = false;  ///< A ".walseq" sidecar now binds the snapshot
  bool wal_truncated = false;           ///< The WAL was truncated up to wal_sequence
};

/**
 * @brief Owner of one snapshot attempt's durability handshake
 *
 * A session is created the moment a snapshot starts writing and models
 * "started but not completed" as a state that cannot be discarded. It leaves
 * that state only through Finish() / TryFinish(), which run the whole
 * handshake, or through the destructor, which runs it on behalf of a caller
 * that dropped the session. There is no representable state in which a
 * snapshot has started, its writer has terminated, and the WAL side effects
 * were skipped.
 *
 * Both modes go through the same object, so their side-effect sets are equal
 * by construction:
 * - fork mode adopts a running child; a completion thread owned by the session
 *   performs the one and only waitpid() for that pid and then runs the
 *   handshake without waiting for a client request, a scheduler tick or
 *   shutdown;
 * - lock mode adopts an already finished in-line write and runs the same
 *   handshake synchronously.
 *
 * Because the child pid is owned by the session and reaped only on its own
 * completion thread, a second waitpid() on that pid — which returns ECHILD and
 * would overwrite a good terminal status with a spurious failure — cannot be
 * expressed.
 *
 * Thread Safety: Thread-safe. TryFinish() / Finish() may be called
 * concurrently and are idempotent; every caller observes the same terminal
 * result.
 */
class SnapshotSession {
 public:
  /**
   * @brief Adopt a fork child that is still writing @p filepath
   *
   * @param filepath Snapshot the child is writing
   * @param child_pid PID of the writing child; this session becomes its only reaper
   * @param wal_sequence WAL sequence captured under the pre-fork barrier (0 when no WAL)
   * @param wal Write-Ahead Log to checkpoint and truncate (may be null)
   */
  SnapshotSession(std::string filepath, pid_t child_pid, uint64_t wal_sequence, WriteAheadLog* wal);

  /**
   * @brief Adopt a snapshot the caller already serialized in-line
   *
   * @param filepath Snapshot that was written
   * @param write_result Terminal result of the in-line write
   * @param wal_sequence WAL sequence captured under the write-lock barrier (0 when no WAL)
   * @param wal Write-Ahead Log to checkpoint and truncate (may be null)
   */
  SnapshotSession(std::string filepath, utils::Expected<void, utils::Error> write_result, uint64_t wal_sequence,
                  WriteAheadLog* wal);

  /// Completes the handshake if the owner did not, then joins the completion thread.
  ~SnapshotSession();

  SnapshotSession(const SnapshotSession&) = delete;
  SnapshotSession& operator=(const SnapshotSession&) = delete;
  SnapshotSession(SnapshotSession&&) = delete;
  SnapshotSession& operator=(SnapshotSession&&) = delete;

  [[nodiscard]] const std::string& filepath() const { return filepath_; }
  [[nodiscard]] pid_t child_pid() const { return child_pid_; }
  [[nodiscard]] uint64_t wal_sequence() const { return wal_sequence_; }

  /// True once the handshake has reached its terminal state.
  [[nodiscard]] bool IsFinished() const;

  /**
   * @brief Complete the handshake without blocking on the writer
   *
   * @return The terminal result, or nullopt while the writer is still running.
   *         Repeated calls after completion return the same result.
   */
  [[nodiscard]] std::optional<utils::Expected<SnapshotOutcome, utils::Error>> TryFinish();

  /**
   * @brief Complete the handshake, waiting for the writer
   *
   * Waits up to @p writer_timeout_ms for a fork child to exit on its own, then
   * asks it to stop (SIGTERM, then SIGKILL after a short grace period) and
   * reaps it. Returns as soon as the handshake reaches its terminal state.
   *
   * @param writer_timeout_ms Grace period for a voluntary exit, in milliseconds
   * @return The terminal result of this session
   */
  [[nodiscard]] utils::Expected<SnapshotOutcome, utils::Error> Finish(uint32_t writer_timeout_ms);

 private:
  /// Blocking reap of the fork child, then the handshake. Runs on completion_thread_.
  void ReapAndComplete();

  /// Run the handshake exactly once and publish its terminal result.
  void Complete(const utils::Expected<void, utils::Error>& write_result);

  /// Sidecar + truncate for a writer that succeeded; nothing for one that failed.
  utils::Expected<SnapshotOutcome, utils::Error> RunDurabilityHandshake(
      const utils::Expected<void, utils::Error>& write_result);

  /// Remove temporary files the fork child left behind when it did not publish.
  void ReclaimChildTemporaries() const;

  const std::string filepath_;
  const pid_t child_pid_;
  const uint64_t wal_sequence_;
  WriteAheadLog* const wal_;

  mutable std::mutex mutex_;
  std::condition_variable finished_cv_;
  bool finished_ = false;
  std::optional<utils::Expected<SnapshotOutcome, utils::Error>> result_;
  /// Set for a lock-mode session until the handshake consumes it.
  std::optional<utils::Expected<void, utils::Error>> pending_write_result_;
  std::thread completion_thread_;
};

}  // namespace nvecd::storage
