/**
 * @file snapshot_session.cpp
 * @brief Snapshot durability handshake implementation
 */

#include "storage/snapshot_session.h"

#include <signal.h>
#include <sys/wait.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <utility>

#include "storage/wal.h"
#include "storage/wal_checkpoint.h"
#include "utils/structured_log.h"

namespace nvecd::storage {

namespace {

/// Grace period between SIGTERM and SIGKILL when a child overruns its deadline.
constexpr uint32_t kChildSigtermGraceMs = 500;

}  // namespace

SnapshotSession::SnapshotSession(std::string filepath, pid_t child_pid, uint64_t wal_sequence, WriteAheadLog* wal)
    : filepath_(std::move(filepath)), child_pid_(child_pid), wal_sequence_(wal_sequence), wal_(wal) {
  completion_thread_ = std::thread(&SnapshotSession::ReapAndComplete, this);
}

SnapshotSession::SnapshotSession(std::string filepath, utils::Expected<void, utils::Error> write_result,
                                 uint64_t wal_sequence, WriteAheadLog* wal)
    : filepath_(std::move(filepath)),
      child_pid_(-1),
      wal_sequence_(wal_sequence),
      wal_(wal),
      pending_write_result_(std::move(write_result)) {}

SnapshotSession::~SnapshotSession() {
  // A session that reached the destructor without being finished is exactly the
  // case that used to leave a snapshot half durable. Complete it here so no
  // exit path can drop the handshake.
  if (!IsFinished()) {
    static_cast<void>(Finish(kChildSigtermGraceMs));
  }
  if (completion_thread_.joinable()) {
    completion_thread_.join();
  }
}

bool SnapshotSession::IsFinished() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return finished_;
}

std::optional<utils::Expected<SnapshotOutcome, utils::Error>> SnapshotSession::TryFinish() {
  std::optional<utils::Expected<void, utils::Error>> pending;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (finished_) {
      return *result_;
    }
    if (!pending_write_result_.has_value()) {
      // Either a fork child is still writing, or another caller already took
      // the in-line result and is running the handshake right now.
      return std::nullopt;
    }
    pending = std::move(pending_write_result_);
    pending_write_result_.reset();
  }

  Complete(*pending);

  std::lock_guard<std::mutex> lock(mutex_);
  return *result_;
}

utils::Expected<SnapshotOutcome, utils::Error> SnapshotSession::Finish(uint32_t writer_timeout_ms) {
  if (auto ready = TryFinish()) {
    return *ready;
  }

  {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!finished_ && writer_timeout_ms > 0) {
      finished_cv_.wait_for(lock, std::chrono::milliseconds(writer_timeout_ms), [this] { return finished_; });
    }
  }

  // The writer overran its deadline. Ask the child to stop, escalate if it does
  // not, and let the completion thread reap it: the session still ends in a
  // terminal state instead of leaking a zombie and an unreclaimed temp file.
  if (child_pid_ > 0 && !IsFinished()) {
    ::kill(child_pid_, SIGTERM);
    utils::LogStorageInfo("snapshot_session",
                          "Sent SIGTERM to snapshot child (pid: " + std::to_string(child_pid_) + ")");
    {
      std::unique_lock<std::mutex> lock(mutex_);
      finished_cv_.wait_for(lock, std::chrono::milliseconds(kChildSigtermGraceMs), [this] { return finished_; });
    }
    if (!IsFinished()) {
      ::kill(child_pid_, SIGKILL);
      utils::LogStorageInfo("snapshot_session",
                            "Sent SIGKILL to snapshot child (pid: " + std::to_string(child_pid_) + ")");
    }
  }

  std::unique_lock<std::mutex> lock(mutex_);
  finished_cv_.wait(lock, [this] { return finished_; });
  return *result_;
}

void SnapshotSession::ReapAndComplete() {
  int status = 0;
  pid_t reaped = -1;
  while (true) {
    reaped = ::waitpid(child_pid_, &status, 0);
    if (reaped >= 0 || errno != EINTR) {
      break;
    }
  }

  utils::Expected<void, utils::Error> write_result;
  if (reaped < 0) {
    // An existing pathname is not proof of success — it may predate this child.
    write_result = utils::MakeUnexpected(utils::MakeError(
        utils::ErrorCode::kSnapshotChildFailed,
        "Snapshot child ownership lost before its exit status was collected: " + std::string(std::strerror(errno)),
        filepath_));
  } else if (WIFEXITED(status)) {
    const int exit_code = WEXITSTATUS(status);
    if (exit_code != kSnapshotChildExitSuccess) {
      write_result = utils::MakeUnexpected(
          utils::MakeError(utils::ErrorCode::kSnapshotChildFailed,
                           "Snapshot child exited with code " + std::to_string(exit_code), filepath_));
    }
  } else if (WIFSIGNALED(status)) {
    write_result = utils::MakeUnexpected(
        utils::MakeError(utils::ErrorCode::kSnapshotChildFailed,
                         "Snapshot child killed by signal " + std::to_string(WTERMSIG(status)), filepath_));
  } else {
    write_result = utils::MakeUnexpected(
        utils::MakeError(utils::ErrorCode::kSnapshotChildFailed, "Snapshot child terminated abnormally", filepath_));
  }

  Complete(write_result);
}

void SnapshotSession::Complete(const utils::Expected<void, utils::Error>& write_result) {
  auto outcome = RunDurabilityHandshake(write_result);

  std::lock_guard<std::mutex> lock(mutex_);
  finished_ = true;
  result_ = std::move(outcome);
  finished_cv_.notify_all();
}

utils::Expected<SnapshotOutcome, utils::Error> SnapshotSession::RunDurabilityHandshake(
    const utils::Expected<void, utils::Error>& write_result) {
  if (!write_result) {
    // Fully failed: reclaim what the writer left behind and leave the WAL
    // untouched, so the recovery base is exactly what it was before the attempt.
    ReclaimChildTemporaries();
    utils::LogStorageError("snapshot_session", filepath_, write_result.error().message());
    return utils::MakeUnexpected(write_result.error());
  }

  SnapshotOutcome outcome;
  outcome.filepath = filepath_;
  outcome.wal_sequence = wal_sequence_;

  if (wal_ == nullptr) {
    utils::LogStorageInfo("snapshot_session", "Snapshot completed: " + filepath_);
    return outcome;
  }

  // Record the checkpoint sidecar first and truncate only after it is durable,
  // so a crash between the two can never leave a WAL that was cut back to a
  // snapshot no reader will accept as a recovery base.
  auto checkpoint = WriteWalCheckpoint(filepath_, wal_sequence_);
  if (!checkpoint) {
    const auto error = utils::MakeError(utils::ErrorCode::kStorageWriteError,
                                        "Failed to write WAL checkpoint: " + checkpoint.error().message(), filepath_);
    utils::LogStorageError("snapshot_session", filepath_, error.message());
    return utils::MakeUnexpected(error);
  }
  outcome.wal_checkpoint_written = true;

  auto truncated = wal_->Truncate(wal_sequence_);
  if (!truncated) {
    const auto error = utils::MakeError(utils::ErrorCode::kWalTruncateFailed,
                                        "Failed to truncate WAL: " + truncated.error().message(), filepath_);
    utils::LogStorageError("snapshot_session", filepath_, error.message());
    return utils::MakeUnexpected(error);
  }
  outcome.wal_truncated = true;

  utils::LogStorageInfo("snapshot_session", "Snapshot completed and WAL truncated: " + filepath_);
  return outcome;
}

void SnapshotSession::ReclaimChildTemporaries() const {
  // Lock mode reclaims its own temporary through SecureTemporaryFile's
  // destructor; only a separate process can die before that runs.
  if (child_pid_ <= 0) {
    return;
  }

  const std::filesystem::path path(filepath_);
  const std::filesystem::path directory = path.has_parent_path() ? path.parent_path() : std::filesystem::path(".");
  // Mirrors PrivateStorageTarget::CreateTemporaryFile: ".<name>.tmp.<pid>.<n>".
  const std::string prefix = "." + path.filename().string() + ".tmp." + std::to_string(child_pid_) + ".";

  std::error_code iterate_error;
  std::filesystem::directory_iterator entries(directory, iterate_error);
  if (iterate_error) {
    return;
  }
  for (const auto& entry : entries) {
    if (entry.path().filename().string().rfind(prefix, 0) != 0) {
      continue;
    }
    std::error_code remove_error;
    std::filesystem::remove(entry.path(), remove_error);
    if (remove_error) {
      utils::LogStorageWarning("snapshot_session", "Failed to reclaim snapshot temporary file '" +
                                                       entry.path().string() + "': " + remove_error.message());
      continue;
    }
    utils::LogStorageInfo("snapshot_session", "Reclaimed snapshot temporary file: " + entry.path().string());
  }
}

}  // namespace nvecd::storage
