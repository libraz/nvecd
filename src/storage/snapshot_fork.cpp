/**
 * @file snapshot_fork.cpp
 * @brief Fork-based COW snapshot writer implementation
 */

#include "storage/snapshot_fork.h"

#include <pthread.h>
#include <signal.h>
#include <spdlog/spdlog.h>
#include <unistd.h>

#include <cstring>
#include <ctime>
#include <memory>
#include <mutex>
#include <new>
#include <shared_mutex>
#include <utility>

#include "storage/snapshot_format_v1.h"
#include "storage/snapshot_session.h"
#include "storage/wal.h"
#include "utils/structured_log.h"

#ifdef __linux__
#include <linux/close_range.h>
#include <sys/syscall.h>
#endif

namespace nvecd::storage {

namespace {
constexpr int kMinInheritedFD = 3;  // Close FDs >= 3 (preserve stdin/stdout/stderr)

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

/**
 * @brief Re-initialize a store lock the child inherited across fork()
 *
 * fork() copies a shared_mutex together with whatever state the parent's other
 * threads had installed in it. A sibling thread parked inside lock() leaves the
 * "writer waiting" flag set, and that thread does not exist in the child to
 * clear it, so the child's next lock_shared() — taken by the serializer's own
 * const getters — blocks forever. The child then never reaches _exit(), the
 * snapshot stays in progress for the life of the process, and every later
 * snapshot is refused while the WAL grows without bound. The window is not
 * theoretical: the parent acquires the barrier, logs, and flushes the logger
 * before it forks, and a writer arriving anywhere in there is enough.
 *
 * The child is single-threaded and the sole observer of these copies, so
 * re-constructing each mutex in place restores a well-defined unlocked state.
 * The lock is released rather than unlocked because its ownership refers to the
 * object whose lifetime the reuse has just ended.
 *
 * @param lock Inherited lock; disassociated and its mutex reset to unlocked
 */
void ResetInheritedLock(std::shared_lock<std::shared_mutex>& lock) {
  std::shared_mutex* mutex = lock.release();
  if (mutex == nullptr) {
    return;
  }
  new (mutex) std::shared_mutex();
}
}  // namespace

ForkSnapshotWriter::~ForkSnapshotWriter() {
  WaitForChild();
}

utils::Expected<void, utils::Error> ForkSnapshotWriter::StartBackgroundSave(
    const std::string& filepath, const config::Config& config, events::EventStore& event_store,
    events::CoOccurrenceIndex& co_index, vectors::VectorStore& vector_store, vectors::MetadataStore* metadata_store) {
  // Release a session that has already finished so a caller who never polled
  // for status is not told that a long-gone snapshot is still in progress.
  CheckChild();
  {
    std::lock_guard lock(status_mutex_);
    if (current_result_.status == SnapshotStatus::kInProgress) {
      return utils::MakeUnexpected(utils::MakeError(
          utils::ErrorCode::kSnapshotAlreadyInProgress,
          "Background snapshot already in progress (pid: " + std::to_string(current_result_.child_pid) + ")"));
    }
    // Reserve ownership before taking store locks or calling fork(). A second
    // caller can no longer pass a status check while this caller is between
    // the check and publishing its child PID.
    current_result_.status = SnapshotStatus::kInProgress;
    current_result_.filepath = filepath;
    current_result_.error_message.clear();
    current_result_.child_pid = -1;
    current_result_.start_time = static_cast<uint64_t>(std::time(nullptr));
    current_result_.end_time = 0;
    current_result_.wal_sequence = 0;
  }

  utils::LogStorageInfo("snapshot_fork", "Acquiring store locks for pre-fork barrier");

  // Install the fork barrier that flushes spdlog before fork (see
  // EnsureAtForkRegistered). Idempotent across snapshots.
  EnsureAtForkRegistered();

  // Pre-fork barrier: hold a shared lock on every store simultaneously. This
  // drains any active writers and excludes new writers until fork has captured
  // the COW image. Shared ownership is what the parent needs; the child does not
  // try to release these copies at all, because their state is whatever the
  // parent's other threads left in them. The child resets them instead — see
  // ResetInheritedLock and ChildProcess.
  auto lock_es = event_store.AcquireReadLock();
  auto lock_co = co_index.AcquireReadLock();
  auto lock_vs = vector_store.AcquireReadLock();
  auto lock_ms = metadata_store != nullptr ? metadata_store->AcquireReadLock() : std::shared_lock<std::shared_mutex>();

  // Capture the WAL sequence WHILE the store-lock barrier is held. Writes are
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
    {
      std::lock_guard lock(status_mutex_);
      current_result_.status = SnapshotStatus::kFailed;
      current_result_.error_message = err;
      current_result_.end_time = static_cast<uint64_t>(std::time(nullptr));
    }
    utils::LogStorageError("snapshot_fork", filepath, err);
    return utils::MakeUnexpected(utils::MakeError(utils::ErrorCode::kSnapshotForkFailed, err));
  }

  if (pid == 0) {
    // ===== Child process =====
    // Surrender every inherited lock to the child entry point, which resets
    // them before touching a store. Unlocking them here instead would keep the
    // parent's "writer waiting" state and deadlock the serializer.
    ChildProcess(InheritedStoreLocks{std::move(lock_es), std::move(lock_co), std::move(lock_vs), std::move(lock_ms)},
                 filepath, config, event_store, co_index, vector_store, metadata_store);
    // UNREACHABLE
  }

  // ===== Parent process =====
  // Release store locks immediately — parent continues serving
  lock_es.unlock();
  lock_co.unlock();
  lock_vs.unlock();
  if (lock_ms.owns_lock()) {
    lock_ms.unlock();
  }

  // Hand the child to a session before returning. From this point the
  // durability handshake has an owner that completes it on its own, without
  // depending on a later client request, on a scheduler that may not exist, or
  // on graceful shutdown.
  {
    std::lock_guard lock(status_mutex_);
    current_result_.child_pid = pid;
    current_result_.wal_sequence = captured_wal_sequence;
    session_ = std::make_shared<SnapshotSession>(filepath, pid, captured_wal_sequence, wal_);
  }

  utils::LogStorageInfo("snapshot_fork", "Fork snapshot started (child pid: " + std::to_string(pid) + ")");

  return {};
}

void ForkSnapshotWriter::ChildProcess(InheritedStoreLocks locks, const std::string& filepath,
                                      const config::Config& config, const events::EventStore& event_store,
                                      const events::CoOccurrenceIndex& co_index,
                                      const vectors::VectorStore& vector_store,
                                      const vectors::MetadataStore* metadata_store) {
  // After fork() in a multithreaded process the child must restrict itself to
  // operations that do not depend on a lock a sibling thread may have held at
  // fork time. In particular it must NOT call into spdlog: the parent flushed
  // and quiesced the logger before fork (see StartBackgroundSave and
  // AtForkPrepare), and any spdlog call here could block forever on a
  // registry/sink mutex inherited in a locked state. All child diagnostics use
  // the async-signal-safe ChildWriteStderr() instead.

  // 0. Reset every inherited store lock before anything can take one. These are
  //    the only locks the serializer acquires, so after this the child cannot
  //    block on a releaser that does not exist here (see ResetInheritedLock).
  ResetInheritedLock(locks.event_store);
  ResetInheritedLock(locks.co_index);
  ResetInheritedLock(locks.vector_store);
  ResetInheritedLock(locks.metadata_store);

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
  _exit(result ? kSnapshotChildExitSuccess : kSnapshotChildExitFailure);
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
  std::shared_ptr<SnapshotSession> session;
  {
    std::lock_guard lock(status_mutex_);
    session = session_;
  }
  if (session == nullptr) {
    return;
  }

  auto outcome = session->TryFinish();
  if (!outcome.has_value()) {
    return;  // The session's writer has not terminated yet.
  }
  PublishOutcome(session, *outcome);
}

void ForkSnapshotWriter::PublishOutcome(const std::shared_ptr<SnapshotSession>& session,
                                        const utils::Expected<SnapshotOutcome, utils::Error>& outcome) {
  {
    std::lock_guard lock(status_mutex_);
    if (session_ != session) {
      return;  // Another caller already published this session's outcome.
    }
    session_.reset();
    current_result_.end_time = static_cast<uint64_t>(std::time(nullptr));
    if (outcome) {
      current_result_.status = SnapshotStatus::kCompleted;
      current_result_.error_message.clear();
    } else {
      current_result_.status = SnapshotStatus::kFailed;
      current_result_.error_message = outcome.error().message();
    }
  }

  if (outcome) {
    utils::LogStorageInfo("snapshot_fork", "Fork snapshot completed: " + session->filepath());
  } else {
    utils::LogStorageError("snapshot_fork", session->filepath(), outcome.error().message());
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
  std::shared_ptr<SnapshotSession> session;
  {
    std::lock_guard lock(status_mutex_);
    session = session_;
  }
  if (session == nullptr) {
    return;
  }

  // Finish() always terminates: it escalates to SIGTERM and then SIGKILL when
  // the child overruns the grace period, so this never blocks forever and the
  // session's terminal outcome always exists by the time it returns.
  auto outcome = session->Finish(timeout_ms);
  PublishOutcome(session, outcome);
}

}  // namespace nvecd::storage
