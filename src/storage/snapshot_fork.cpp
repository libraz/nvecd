/**
 * @file snapshot_fork.cpp
 * @brief Fork-based COW snapshot writer implementation
 */

#include "storage/snapshot_fork.h"

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <ctime>
#include <filesystem>
#include <thread>

#include <spdlog/spdlog.h>

#include "storage/snapshot_format_v1.h"
#include "utils/structured_log.h"

#ifdef __linux__
#include <linux/close_range.h>
#include <sys/syscall.h>
#endif

namespace nvecd::storage {

namespace {
constexpr int kMinInheritedFD =
    3;  // Close FDs >= 3 (preserve stdin/stdout/stderr)
constexpr int kChildExitSuccess = 0;
constexpr int kChildExitFailure = 1;
}  // namespace

ForkSnapshotWriter::~ForkSnapshotWriter() {
  WaitForChild(5000);  // NOLINT(cppcoreguidelines-avoid-magic-numbers)
}

utils::Expected<void, utils::Error> ForkSnapshotWriter::StartBackgroundSave(
    const std::string& filepath, const config::Config& config,
    events::EventStore& event_store, events::CoOccurrenceIndex& co_index,
    vectors::VectorStore& vector_store) {
  {
    std::lock_guard lock(status_mutex_);
    if (current_result_.status == SnapshotStatus::kInProgress) {
      return utils::MakeUnexpected(utils::MakeError(
          utils::ErrorCode::kSnapshotAlreadyInProgress,
          "Background snapshot already in progress (pid: " +
              std::to_string(current_result_.child_pid) + ")"));
    }
  }

  utils::LogStorageInfo("snapshot_fork",
                        "Acquiring write locks for pre-fork barrier");

  // Pre-fork barrier: acquire all write locks to ensure consistent mutex state
  auto lock_es = event_store.AcquireWriteLock();
  auto lock_co = co_index.AcquireWriteLock();
  auto lock_vs = vector_store.AcquireWriteLock();

  // Ensure SIGCHLD is not SIG_IGN (macOS auto-reaps children when ignored)
  signal(SIGCHLD, SIG_DFL);  // NOLINT(cppcoreguidelines-pro-type-cstyle-cast)

  utils::LogStorageInfo("snapshot_fork",
                        "Forking child process for snapshot: " + filepath);

  pid_t pid = fork();

  if (pid < 0) {
    // fork failed — locks released by RAII
    std::string err = "fork() failed: " + std::string(strerror(errno));
    utils::LogStorageError("snapshot_fork", filepath, err);
    return utils::MakeUnexpected(
        utils::MakeError(utils::ErrorCode::kSnapshotForkFailed, err));
  }

  if (pid == 0) {
    // ===== Child process =====
    // RAII lock guards will unlock in child — this is safe because
    // child is single-threaded and these are copies of parent's mutexes.
    // Release locks explicitly before doing any work.
    lock_es.unlock();
    lock_co.unlock();
    lock_vs.unlock();

    // Enter child process (never returns)
    ChildProcess(filepath, config, event_store, co_index, vector_store);
    // UNREACHABLE
  }

  // ===== Parent process =====
  // Release write locks immediately — parent continues serving
  lock_es.unlock();
  lock_co.unlock();
  lock_vs.unlock();

  // Update status
  {
    std::lock_guard lock(status_mutex_);
    current_result_.status = SnapshotStatus::kInProgress;
    current_result_.filepath = filepath;
    current_result_.error_message.clear();
    current_result_.child_pid = pid;
    current_result_.start_time = static_cast<uint64_t>(std::time(nullptr));
    current_result_.end_time = 0;
  }

  utils::LogStorageInfo(
      "snapshot_fork",
      "Fork snapshot started (child pid: " + std::to_string(pid) + ")");

  return {};
}

void ForkSnapshotWriter::ChildProcess(
    const std::string& filepath, const config::Config& config,
    const events::EventStore& event_store,
    const events::CoOccurrenceIndex& co_index,
    const vectors::VectorStore& vector_store) {
  // 1. Close inherited file descriptors (server sockets, log files, etc.)
  CloseInheritedFDs(kMinInheritedFD);

  // 2. Shutdown spdlog to avoid writing to parent's log file descriptors
  spdlog::shutdown();

  // 3. Reset signal handlers
  signal(SIGCHLD, SIG_DFL);  // NOLINT(cppcoreguidelines-pro-type-cstyle-cast)
  signal(SIGPIPE, SIG_DFL);  // NOLINT(cppcoreguidelines-pro-type-cstyle-cast)
  signal(SIGTERM, SIG_DFL);  // NOLINT(cppcoreguidelines-pro-type-cstyle-cast)

  // 4. Write snapshot — data is a frozen COW copy from parent
  auto result = snapshot_v1::WriteSnapshotV1(filepath, config, event_store,
                                             co_index, vector_store);

  // 5. Exit (never call exit() — use _exit() to avoid atexit handlers)
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

  std::lock_guard lock(status_mutex_);
  current_result_.end_time = static_cast<uint64_t>(std::time(nullptr));

  if (result < 0) {
    if (errno == ECHILD) {
      // Child was already reaped (e.g., SIGCHLD was SIG_IGN or handled externally).
      // Determine success by checking if the snapshot file exists.
      if (std::filesystem::exists(current_result_.filepath)) {
        current_result_.status = SnapshotStatus::kCompleted;
        utils::LogStorageInfo(
            "snapshot_fork",
            "Fork snapshot completed (auto-reaped): " + current_result_.filepath);
      } else {
        current_result_.status = SnapshotStatus::kFailed;
        current_result_.error_message = "Child was auto-reaped and snapshot file not found";
        utils::LogStorageError("snapshot_fork", current_result_.filepath,
                               current_result_.error_message);
      }
    } else {
      current_result_.status = SnapshotStatus::kFailed;
      current_result_.error_message =
          "waitpid failed: " + std::string(strerror(errno));
      utils::LogStorageError("snapshot_fork", current_result_.filepath,
                             current_result_.error_message);
    }
    return;
  }

  // Child exited
  if (WIFEXITED(status)) {
    int exit_code = WEXITSTATUS(status);
    if (exit_code == kChildExitSuccess) {
      current_result_.status = SnapshotStatus::kCompleted;
      utils::LogStorageInfo(
          "snapshot_fork",
          "Fork snapshot completed: " + current_result_.filepath);
    } else {
      current_result_.status = SnapshotStatus::kFailed;
      current_result_.error_message =
          "Child exited with code " + std::to_string(exit_code);
      utils::LogStorageError("snapshot_fork", current_result_.filepath,
                             current_result_.error_message);
    }
  } else if (WIFSIGNALED(status)) {
    int sig = WTERMSIG(status);
    current_result_.status = SnapshotStatus::kFailed;
    current_result_.error_message =
        "Child killed by signal " + std::to_string(sig);
    utils::LogStorageError("snapshot_fork", current_result_.filepath,
                           current_result_.error_message);
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
  utils::LogStorageInfo(
      "snapshot_fork",
      "Sent SIGTERM to snapshot child (pid: " + std::to_string(child_pid) +
          ")");

  // Wait a bit more, then reap
  std::this_thread::sleep_for(std::chrono::milliseconds(kPollIntervalMs));
  CheckChild();
}

}  // namespace nvecd::storage
