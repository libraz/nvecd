/**
 * @file snapshot_fork.cpp
 * @brief Fork-based COW snapshot writer implementation
 */

#include "storage/snapshot_fork.h"

#include <pthread.h>
#include <signal.h>
#include <spdlog/spdlog.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <thread>

#include "storage/snapshot_format_v1.h"
#include "storage/wal.h"
#include "storage/wal_checkpoint.h"
#include "utils/structured_log.h"

#ifdef __linux__
#include <linux/close_range.h>
#include <sys/syscall.h>
#endif

namespace nvecd::storage {

namespace {
constexpr int kMinInheritedFD = 3;  // Close FDs >= 3 (preserve stdin/stdout/stderr)
constexpr int kChildExitSuccess = 0;
constexpr int kChildExitFailure = 1;

/**
 * @brief Async-signal-safe error report to stderr.
 *
 * Used exclusively on the post-fork child path. After fork() in a
 * multithreaded process, the only operations the child may safely perform are
 * async-signal-safe ones, because any non-async-signal-safe lock (the libc
 * allocator arena, the spdlog registry/sink mutex, ...) may have been held by a
 * sibling thread at fork time and is now permanently locked in the child.
 * write(2) is async-signal-safe and does not depend on any such lock, so it is
 * the only logging primitive the child uses.
 *
 * @param msg NUL-terminated message (must be a compile-time/heap-free literal).
 */
void ChildWriteStderr(const char* msg) {
  // Ignore the result: there is nothing actionable the child can do on a failed
  // write, and it must not allocate or branch into non-async-signal-safe code.
  const ssize_t written = write(STDERR_FILENO, msg, std::strlen(msg));
  static_cast<void>(written);
}

/**
 * @brief pthread_atfork "prepare" handler.
 *
 * Runs in the forking thread while the process is still multithreaded, before
 * fork() snapshots the address space. Flushing the logger here drains any
 * buffered records so they are not lost or duplicated across the fork, and
 * leaves the spdlog sinks in a quiescent state. The child never touches spdlog
 * again (see ChildProcess), so no spdlog lock can deadlock the child.
 */
void AtForkPrepare() {
  spdlog::details::registry::instance().flush_all();
}

/**
 * @brief Register the pthread_atfork handlers exactly once.
 *
 * The parent/child post-fork handlers are intentionally no-ops: the parent's
 * logger is already consistent, and the child must not re-enter spdlog. The
 * sole job of the registration is to install the "prepare" flush as a barrier.
 */
void EnsureAtForkRegistered() {
  static std::once_flag once;
  std::call_once(once, [] { pthread_atfork(&AtForkPrepare, nullptr, nullptr); });
}
}  // namespace

ForkSnapshotWriter::~ForkSnapshotWriter() {
  WaitForChild(5000);  // NOLINT(cppcoreguidelines-avoid-magic-numbers)
}

utils::Expected<void, utils::Error> ForkSnapshotWriter::StartBackgroundSave(
    const std::string& filepath, const config::Config& config, events::EventStore& event_store,
    events::CoOccurrenceIndex& co_index, vectors::VectorStore& vector_store, vectors::MetadataStore* metadata_store) {
  {
    std::lock_guard lock(status_mutex_);
    if (current_result_.status == SnapshotStatus::kInProgress) {
      return utils::MakeUnexpected(utils::MakeError(
          utils::ErrorCode::kSnapshotAlreadyInProgress,
          "Background snapshot already in progress (pid: " + std::to_string(current_result_.child_pid) + ")"));
    }
  }

  utils::LogStorageInfo("snapshot_fork", "Acquiring write locks for pre-fork barrier");

  // Install the fork barrier that flushes spdlog before fork (see
  // EnsureAtForkRegistered). Idempotent across snapshots.
  EnsureAtForkRegistered();

  // Pre-fork barrier: acquire all write locks to ensure consistent mutex state
  auto lock_es = event_store.AcquireWriteLock();
  auto lock_co = co_index.AcquireWriteLock();
  auto lock_vs = vector_store.AcquireWriteLock();
  auto lock_ms = metadata_store != nullptr ? metadata_store->AcquireWriteLock() : std::unique_lock<std::shared_mutex>();

  // Capture the WAL sequence WHILE the write-lock barrier is held. Writes are
  // serialized behind these locks, so the captured value is exactly the maximum
  // op reflected in the about-to-be-frozen (COW) snapshot. It is recorded in the
  // checkpoint sidecar and used to truncate the WAL only after the child
  // succeeds, so the WAL never drops a record the snapshot does not contain.
  const uint64_t captured_wal_sequence = (wal_ != nullptr) ? wal_->CurrentSequence() : 0;

  // Ensure SIGCHLD is not SIG_IGN (macOS auto-reaps children when ignored)
  signal(SIGCHLD, SIG_DFL);  // NOLINT(cppcoreguidelines-pro-type-cstyle-cast)

  utils::LogStorageInfo("snapshot_fork", "Forking child process for snapshot: " + filepath);

  // Flush the logger immediately before fork so the child inherits no buffered
  // log records and never has to re-enter spdlog (which would risk deadlocking
  // on a registry/sink mutex held by a sibling thread at fork time).
  spdlog::details::registry::instance().flush_all();

  pid_t pid = fork();

  if (pid < 0) {
    // fork failed — locks released by RAII
    std::string err = "fork() failed: " + std::string(strerror(errno));
    utils::LogStorageError("snapshot_fork", filepath, err);
    return utils::MakeUnexpected(utils::MakeError(utils::ErrorCode::kSnapshotForkFailed, err));
  }

  if (pid == 0) {
    // ===== Child process =====
    // RAII lock guards will unlock in child — this is safe because
    // child is single-threaded and these are copies of parent's mutexes.
    // Release locks explicitly before doing any work.
    lock_es.unlock();
    lock_co.unlock();
    lock_vs.unlock();
    if (lock_ms.owns_lock()) {
      lock_ms.unlock();
    }

    // Enter child process (never returns)
    ChildProcess(filepath, config, event_store, co_index, vector_store, metadata_store);
    // UNREACHABLE
  }

  // ===== Parent process =====
  // Release write locks immediately — parent continues serving
  lock_es.unlock();
  lock_co.unlock();
  lock_vs.unlock();
  if (lock_ms.owns_lock()) {
    lock_ms.unlock();
  }

  // Update status
  {
    std::lock_guard lock(status_mutex_);
    current_result_.status = SnapshotStatus::kInProgress;
    current_result_.filepath = filepath;
    current_result_.error_message.clear();
    current_result_.child_pid = pid;
    current_result_.start_time = static_cast<uint64_t>(std::time(nullptr));
    current_result_.end_time = 0;
    current_result_.wal_sequence = captured_wal_sequence;
  }

  utils::LogStorageInfo("snapshot_fork", "Fork snapshot started (child pid: " + std::to_string(pid) + ")");

  return {};
}

void ForkSnapshotWriter::ChildProcess(const std::string& filepath, const config::Config& config,
                                      const events::EventStore& event_store, const events::CoOccurrenceIndex& co_index,
                                      const vectors::VectorStore& vector_store,
                                      const vectors::MetadataStore* metadata_store) {
  // After fork() in a multithreaded process the child must restrict itself to
  // operations that do not depend on a lock a sibling thread may have held at
  // fork time. In particular it must NOT call into spdlog: the parent flushed
  // and quiesced the logger before fork (see StartBackgroundSave and
  // AtForkPrepare), and any spdlog call here could block forever on a
  // registry/sink mutex inherited in a locked state. All child diagnostics use
  // the async-signal-safe ChildWriteStderr() instead.

  // 1. Close inherited file descriptors (server sockets, log files, etc.).
  //    This also drops the child's copies of the parent's log sink FDs, so the
  //    child cannot corrupt the parent's log output.
  CloseInheritedFDs(kMinInheritedFD);

  // 2. Reset signal handlers
  signal(SIGCHLD, SIG_DFL);  // NOLINT(cppcoreguidelines-pro-type-cstyle-cast)
  signal(SIGPIPE, SIG_DFL);  // NOLINT(cppcoreguidelines-pro-type-cstyle-cast)
  signal(SIGTERM, SIG_DFL);  // NOLINT(cppcoreguidelines-pro-type-cstyle-cast)

  // 3. Write snapshot — data is a frozen COW copy from parent.
  //    WriteSnapshotV1 may allocate via the libc allocator; that is safe across
  //    fork because libc registers its own pthread_atfork handlers for the
  //    allocator arenas. It must not, however, log via spdlog on its error
  //    path; failures are surfaced through the exit code below and reported by
  //    the parent in CheckChild().
  auto result = snapshot_v1::WriteSnapshotV1(filepath, config, event_store, co_index, vector_store, nullptr, nullptr,
                                             metadata_store, /*suppress_logging=*/true);

  if (!result) {
    ChildWriteStderr("nvecd: fork snapshot child failed to write snapshot\n");
  }

  // 4. Exit (never call exit() — use _exit() to avoid atexit handlers)
  _exit(result ? kChildExitSuccess : kChildExitFailure);
}

void ForkSnapshotWriter::CloseInheritedFDs(int min_fd) {
#ifdef __linux__
  // Linux 5.9+: close_range syscall
  if (syscall(SYS_close_range, min_fd, ~0U, 0) == 0) {
    return;
  }
  // Fallback: iterate
#endif

  // macOS and Linux fallback: iterate up to getdtablesize()
  int max_fd = getdtablesize();
  for (int fd = min_fd; fd < max_fd; ++fd) {
    close(fd);
  }
}

void ForkSnapshotWriter::CheckChild() {
  pid_t child_pid;
  {
    std::lock_guard lock(status_mutex_);
    if (current_result_.status != SnapshotStatus::kInProgress) {
      return;
    }
    child_pid = current_result_.child_pid;
  }

  int status = 0;
  pid_t result = waitpid(child_pid, &status, WNOHANG);

  if (result == 0) {
    return;  // Child still running
  }

  bool completed = false;
  std::string completed_filepath;
  uint64_t completed_wal_sequence = 0;
  {
    std::lock_guard lock(status_mutex_);
    current_result_.end_time = static_cast<uint64_t>(std::time(nullptr));

    if (result < 0) {
      if (errno == ECHILD) {
        // Child was already reaped (e.g., SIGCHLD was SIG_IGN or handled externally).
        // Determine success by checking if the snapshot file exists.
        if (std::filesystem::exists(current_result_.filepath)) {
          current_result_.status = SnapshotStatus::kCompleted;
          utils::LogStorageInfo("snapshot_fork", "Fork snapshot completed (auto-reaped): " + current_result_.filepath);
        } else {
          current_result_.status = SnapshotStatus::kFailed;
          current_result_.error_message = "Child was auto-reaped and snapshot file not found";
          utils::LogStorageError("snapshot_fork", current_result_.filepath, current_result_.error_message);
        }
      } else {
        current_result_.status = SnapshotStatus::kFailed;
        current_result_.error_message = "waitpid failed: " + std::string(strerror(errno));
        utils::LogStorageError("snapshot_fork", current_result_.filepath, current_result_.error_message);
      }
    } else if (WIFEXITED(status)) {
      int exit_code = WEXITSTATUS(status);
      if (exit_code == kChildExitSuccess) {
        current_result_.status = SnapshotStatus::kCompleted;
        utils::LogStorageInfo("snapshot_fork", "Fork snapshot completed: " + current_result_.filepath);
      } else {
        current_result_.status = SnapshotStatus::kFailed;
        current_result_.error_message = "Child exited with code " + std::to_string(exit_code);
        utils::LogStorageError("snapshot_fork", current_result_.filepath, current_result_.error_message);
      }
    } else if (WIFSIGNALED(status)) {
      int sig = WTERMSIG(status);
      current_result_.status = SnapshotStatus::kFailed;
      current_result_.error_message = "Child killed by signal " + std::to_string(sig);
      utils::LogStorageError("snapshot_fork", current_result_.filepath, current_result_.error_message);
    }

    completed = (current_result_.status == SnapshotStatus::kCompleted);
    completed_filepath = current_result_.filepath;
    completed_wal_sequence = current_result_.wal_sequence;
  }

  // On a successful snapshot, record the checkpoint sidecar then truncate the
  // WAL up to the sequence captured under the pre-fork barrier. Truncation only
  // removes WAL files whose records are entirely contained in the snapshot, so
  // any record beyond the captured sequence is preserved for the next recovery.
  if (completed && wal_ != nullptr) {
    auto checkpoint = WriteWalCheckpoint(completed_filepath, completed_wal_sequence);
    if (!checkpoint) {
      utils::LogStorageError("snapshot_fork", completed_filepath,
                             "Failed to write WAL checkpoint: " + checkpoint.error().message());
      return;  // Do not truncate without a durable checkpoint.
    }
    auto truncated = wal_->Truncate(completed_wal_sequence);
    if (!truncated) {
      utils::LogStorageError("snapshot_fork", completed_filepath,
                             "Failed to truncate WAL: " + truncated.error().message());
    }
  }
}

SnapshotResult ForkSnapshotWriter::GetStatus() const {
  std::lock_guard lock(status_mutex_);
  return current_result_;
}

bool ForkSnapshotWriter::IsInProgress() const {
  std::lock_guard lock(status_mutex_);
  return current_result_.status == SnapshotStatus::kInProgress;
}

void ForkSnapshotWriter::WaitForChild(uint32_t timeout_ms) {
  pid_t child_pid;
  {
    std::lock_guard lock(status_mutex_);
    if (current_result_.status != SnapshotStatus::kInProgress) {
      return;
    }
    child_pid = current_result_.child_pid;
  }

  // Poll with short sleeps
  constexpr uint32_t kPollIntervalMs = 100;
  uint32_t elapsed = 0;
  while (elapsed < timeout_ms) {
    int status = 0;
    pid_t result = waitpid(child_pid, &status, WNOHANG);
    if (result != 0) {
      // Child exited or error
      CheckChild();
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(kPollIntervalMs));
    elapsed += kPollIntervalMs;
  }

  // Timeout — send SIGTERM
  kill(child_pid, SIGTERM);
  utils::LogStorageInfo("snapshot_fork", "Sent SIGTERM to snapshot child (pid: " + std::to_string(child_pid) + ")");

  // Wait a bit more, then reap
  std::this_thread::sleep_for(std::chrono::milliseconds(kPollIntervalMs));
  CheckChild();
}

}  // namespace nvecd::storage
