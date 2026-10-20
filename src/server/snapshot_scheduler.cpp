/**
 * @file snapshot_scheduler.cpp
 * @brief Implementation of SnapshotScheduler
 *
 * Reference: ../mygram-db/src/server/snapshot_scheduler.cpp
 * Reusability: 75% (removed MySQL/GTID/TableCatalog, uses ForkSnapshotWriter)
 */

#include "server/snapshot_scheduler.h"

#include <signal.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <system_error>

#include "server/handlers/dump_handler.h"
#include "storage/snapshot_fork.h"
#include "storage/wal_checkpoint.h"
#include "utils/structured_log.h"

namespace nvecd::server {

namespace {
constexpr int kTickIntervalMs = 1000;  // Cadence for polling a finished fork session

/**
 * @brief True for a snapshot temporary file whose writing process is gone
 *
 * PrivateStorageTarget::CreateTemporaryFile names temporaries
 * ".<snapshot>.tmp.<pid>.<n>". A temporary owned by a live process is left
 * alone; one whose owner no longer exists can never be published, so it is
 * reclaimable. With @p own_pid_is_abandoned a temporary carrying this
 * process's PID is reclaimable too.
 */
bool IsAbandonedTemporary(const std::string& filename, bool own_pid_is_abandoned) {
  static constexpr char kMarker[] = ".tmp.";
  if (filename.empty() || filename.front() != '.') {
    return false;
  }
  const size_t marker = filename.rfind(kMarker);
  if (marker == std::string::npos) {
    return false;
  }
  const size_t pid_start = marker + (sizeof(kMarker) - 1);
  const size_t pid_end = filename.find('.', pid_start);
  if (pid_end == std::string::npos || pid_end == pid_start || pid_end + 1 >= filename.size()) {
    return false;
  }
  const auto all_digits = [](const std::string& text) {
    return std::all_of(text.begin(), text.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
  };
  const std::string pid_text = filename.substr(pid_start, pid_end - pid_start);
  if (!all_digits(pid_text) || !all_digits(filename.substr(pid_end + 1))) {
    return false;
  }

  errno = 0;
  const long owner_pid = std::strtol(pid_text.c_str(), nullptr, 10);
  if (errno != 0 || owner_pid <= 0 || owner_pid > std::numeric_limits<pid_t>::max()) {
    return false;
  }
  if (owner_pid == ::getpid()) {
    return own_pid_is_abandoned;
  }
  errno = 0;
  return ::kill(static_cast<pid_t>(owner_pid), 0) != 0 && errno == ESRCH;
}
}  // namespace

SnapshotScheduler::SnapshotScheduler(config::SnapshotConfig config, HandlerContext& ctx)
    : config_(std::move(config)), ctx_(ctx) {}

SnapshotScheduler::~SnapshotScheduler() {
  Stop();
}

void SnapshotScheduler::Start() {
  if (running_) {
    utils::StructuredLog()
        .Event("server_warning")
        .Field("component", "snapshot_scheduler")
        .Field("type", "already_running")
        .Warn();
    return;
  }

  if (config_.interval_sec <= 0) {
    utils::StructuredLog().Event("snapshot_scheduler_disabled").Field("reason", "interval_sec <= 0").Info();
    return;
  }

  utils::StructuredLog()
      .Event("snapshot_scheduler_starting")
      .Field("interval_sec", static_cast<uint64_t>(config_.interval_sec))
      .Field("retain", static_cast<uint64_t>(config_.retain))
      .Info();

  running_ = true;
  next_save_time_ = std::chrono::steady_clock::now() + std::chrono::seconds(config_.interval_sec);
  worker_.Start(std::chrono::milliseconds(kTickIntervalMs), [this]() { Tick(); });
}

void SnapshotScheduler::Stop() {
  if (!running_) {
    return;
  }

  utils::StructuredLog().Event("snapshot_scheduler_stopping").Info();
  running_ = false;

  worker_.Stop();

  // Drain a final CheckChild on shutdown so a snapshot that completed just
  // before Stop() is published and its retention pass runs.
  CheckFinishedSnapshot();

  utils::StructuredLog().Event("snapshot_scheduler_stopped").Info();
}

void SnapshotScheduler::CheckFinishedSnapshot() {
  if (ctx_.fork_snapshot_writer == nullptr) {
    return;
  }
  const bool was_in_progress = ctx_.fork_snapshot_writer->IsInProgress();
  ctx_.fork_snapshot_writer->CheckChild();
  // A forked child creates its output asynchronously. Retention cleanup
  // must run only after that child is reaped; otherwise the newly started
  // snapshot can appear after CleanupOldSnapshots() has counted files and
  // leave retain + 1 files behind.
  if (was_in_progress && !ctx_.fork_snapshot_writer->IsInProgress()) {
    cleanup_pending_ = true;
  }
  if (cleanup_pending_) {
    cleanup_pending_ = !CleanupOldSnapshots();
  }
}

void SnapshotScheduler::Tick() {
  // Publish the outcome of a finished session every tick. This is the only
  // place the auto-snapshot path drives ForkSnapshotWriter::CheckChild().
  // Reaping the child, writing the WAL checkpoint sidecar and truncating the
  // WAL all happen on the session's own completion thread and do not depend
  // on this call; CheckChild() only picks up the finished outcome and clears
  // the session. Without it the writer stays kInProgress forever, so a single
  // fork would be the only snapshot the process ever takes.
  CheckFinishedSnapshot();

  if (std::chrono::steady_clock::now() >= next_save_time_ && TakeSnapshot()) {
    next_save_time_ = std::chrono::steady_clock::now() + std::chrono::seconds(config_.interval_sec);
  }
}

bool SnapshotScheduler::TakeSnapshot() {
  try {
    // Generate timestamp-based filename
    auto timestamp = std::time(nullptr);
    std::tm tm_buf{};
    localtime_r(&timestamp, &tm_buf);  // Thread-safe version of localtime
    std::ostringstream filename;
    filename << "auto_" << std::put_time(&tm_buf, "%Y%m%d_%H%M%S") << ".nvec";

    std::filesystem::path snapshot_path = std::filesystem::path(config_.dir) / filename.str();

    utils::StructuredLog().Event("snapshot_taking").Field("path", snapshot_path.string()).Info();

    auto result = handlers::StartForkSnapshot(ctx_, snapshot_path.string());

    if (result) {
      utils::StructuredLog().Event("snapshot_started").Field("path", snapshot_path.string()).Info();
      return true;
    }
    // A running save, load or child makes this tick fail; the next tick retries.
    if (result.error().code() == utils::ErrorCode::kSnapshotAlreadyInProgress) {
      utils::StructuredLog()
          .Event("snapshot_deferred")
          .Field("path", snapshot_path.string())
          .Field("reason", result.error().message())
          .Info();
      return false;
    }
    utils::StructuredLog()
        .Event("server_error")
        .Field("operation", "snapshot_save")
        .Field("filepath", snapshot_path.string())
        .Field("error", result.error().message())
        .Error();

  } catch (const std::exception& e) {
    utils::StructuredLog()
        .Event("server_error")
        .Field("operation", "snapshot_save")
        .Field("type", "exception")
        .Field("error", e.what())
        .Error();
  }
  return true;
}

void ReclaimAbandonedTemporaries(const std::string& dir, bool own_pid_is_abandoned) {
  std::error_code error;
  const std::filesystem::path snapshot_dir(dir);
  if (!std::filesystem::is_directory(snapshot_dir, error)) {
    return;
  }
  for (const auto& entry : std::filesystem::directory_iterator(snapshot_dir, error)) {
    if (!entry.is_regular_file(error) ||
        !IsAbandonedTemporary(entry.path().filename().string(), own_pid_is_abandoned)) {
      continue;
    }
    utils::StructuredLog().Event("snapshot_removing_abandoned_temp").Field("path", entry.path().string()).Info();
    std::error_code remove_error;
    std::filesystem::remove(entry.path(), remove_error);
  }
}

bool SnapshotScheduler::CleanupOldSnapshots() {
  // Retention runs as a brief session, so no save or load can bind or touch
  // a file between the ranking below and its removal.
  auto session = ctx_.snapshot_coordinator.BeginBriefSession();
  if (!session) {
    return false;
  }

  // Reclaim before the retention check: temporaries are dot-prefixed and carry
  // the writing process id, so the retention scan never sees them, and a
  // `retain` of 0 would otherwise skip the sweep entirely and let a killed
  // writer's full-size file accumulate on every interrupted run. A lock-mode
  // save in this process may own one, so this process's temporaries stay.
  ReclaimAbandonedTemporaries(config_.dir, /*own_pid_is_abandoned=*/false);

  if (config_.retain <= 0) {
    return true;
  }

  try {
    // Only a recovery base counts toward retain: with the WAL enabled an
    // auto-snapshot whose sidecar does not validate is removed rather than
    // allowed to push the base the WAL was truncated against out of the set.
    const bool require_checkpoint = ctx_.wal != nullptr;
    const auto retain_count = static_cast<size_t>(config_.retain);
    size_t kept = 0;
    for (const auto& candidate : storage::ListRecoveryCandidates(config_.dir, /*require_checkpoint=*/false)) {
      const std::filesystem::path path(candidate);
      if (path.extension() != ".nvec" || path.filename().string().rfind("auto_", 0) != 0) {
        continue;
      }
      if (kept < retain_count && storage::RecoveryCheckpoint(candidate, require_checkpoint)) {
        ++kept;
        continue;
      }

      utils::StructuredLog().Event("snapshot_removing_old").Field("path", candidate).Info();
      // The checkpoint sidecar goes first: a crash between the two removals
      // then leaves a snapshot without a sidecar, never an orphan sidecar.
      std::error_code remove_error;
      std::filesystem::remove(storage::WalCheckpointPath(candidate), remove_error);
      if (!remove_error) {
        std::filesystem::remove(path, remove_error);
      }
      if (remove_error) {
        utils::StructuredLog()
            .Event("server_error")
            .Field("operation", "snapshot_cleanup")
            .Field("path", candidate)
            .Field("error", remove_error.message())
            .Error();
      }
    }

  } catch (const std::exception& e) {
    utils::StructuredLog()
        .Event("server_error")
        .Field("operation", "snapshot_cleanup")
        .Field("type", "exception")
        .Field("error", e.what())
        .Error();
  }
  return true;
}

}  // namespace nvecd::server
