/**
 * @file snapshot_scheduler.cpp
 * @brief Implementation of SnapshotScheduler
 *
 * Reference: ../mygram-db/src/server/snapshot_scheduler.cpp
 * Reusability: 75% (removed MySQL/GTID/TableCatalog, uses ForkSnapshotWriter)
 */

#include "server/snapshot_scheduler.h"

#include <signal.h>

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

#include "utils/flag_guard.h"
#include "utils/structured_log.h"

namespace nvecd::server {

namespace {
constexpr int kCheckIntervalMs = 1000;  // Check for shutdown every second

/**
 * @brief True for a snapshot temporary file whose writing process is gone
 *
 * PrivateStorageTarget::CreateTemporaryFile names temporaries
 * ".<snapshot>.tmp.<pid>.<n>". A temporary owned by a live process is left
 * alone; one whose owner no longer exists can never be published, so it is
 * reclaimable.
 */
bool IsAbandonedTemporary(const std::string& filename) {
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
  errno = 0;
  return ::kill(static_cast<pid_t>(owner_pid), 0) != 0 && errno == ESRCH;
}
}  // namespace

SnapshotScheduler::SnapshotScheduler(config::SnapshotConfig config, storage::ForkSnapshotWriter* fork_writer,
                                     const config::Config* full_config, events::EventStore* event_store,
                                     events::CoOccurrenceIndex* co_index, vectors::VectorStore* vector_store,
                                     vectors::MetadataStore* metadata_store, std::atomic<bool>& read_only)
    : config_(std::move(config)),
      fork_writer_(fork_writer),
      full_config_(full_config),
      event_store_(event_store),
      co_index_(co_index),
      vector_store_(vector_store),
      metadata_store_(metadata_store),
      read_only_(read_only) {}

SnapshotScheduler::SnapshotScheduler(config::SnapshotConfig config, storage::ForkSnapshotWriter* fork_writer,
                                     const config::Config* full_config, events::EventStore* event_store,
                                     events::CoOccurrenceIndex* co_index, vectors::VectorStore* vector_store,
                                     std::atomic<bool>& read_only)
    : SnapshotScheduler(std::move(config), fork_writer, full_config, event_store, co_index, vector_store, nullptr,
                        read_only) {}

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
  scheduler_thread_ = std::make_unique<std::thread>(&SnapshotScheduler::SchedulerLoop, this);
}

void SnapshotScheduler::Stop() {
  if (!running_) {
    return;
  }

  utils::StructuredLog().Event("snapshot_scheduler_stopping").Info();
  running_ = false;

  if (scheduler_thread_ && scheduler_thread_->joinable()) {
    scheduler_thread_->join();
  }

  utils::StructuredLog().Event("snapshot_scheduler_stopped").Info();
}

void SnapshotScheduler::SchedulerLoop() {
  const int interval_sec = config_.interval_sec;

  utils::StructuredLog().Event("snapshot_scheduler_thread_started").Info();

  // Calculate next save time
  auto next_save_time = std::chrono::steady_clock::now() + std::chrono::seconds(interval_sec);

  while (running_) {
    // Reap any finished fork child every tick. This is the only place the
    // auto-snapshot path drives ForkSnapshotWriter::CheckChild(), which reaps
    // the child (preventing zombies), flips status out of kInProgress (so the
    // next snapshot can start), and — critically — writes the WAL checkpoint
    // sidecar and truncates the WAL. Without it a single fork would stay
    // kInProgress forever: only one snapshot per process, WAL growing unbounded.
    if (fork_writer_ != nullptr) {
      const bool was_in_progress = fork_writer_->IsInProgress();
      fork_writer_->CheckChild();
      // A forked child creates its output asynchronously. Retention cleanup
      // must run only after that child is reaped; otherwise the newly started
      // snapshot can appear after CleanupOldSnapshots() has counted files and
      // leave retain + 1 files behind.
      if (was_in_progress && !fork_writer_->IsInProgress()) {
        CleanupOldSnapshots();
      }
    }

    auto now = std::chrono::steady_clock::now();

    // Check if it's time to save
    if (now >= next_save_time) {
      TakeSnapshot();

      // Schedule next save
      next_save_time = std::chrono::steady_clock::now() + std::chrono::seconds(interval_sec);
    }

    // Sleep for check interval
    std::this_thread::sleep_for(std::chrono::milliseconds(kCheckIntervalMs));
  }

  // Drain a final CheckChild on shutdown so a snapshot that completed just
  // before Stop() still records its checkpoint and truncates the WAL.
  if (fork_writer_ != nullptr) {
    const bool was_in_progress = fork_writer_->IsInProgress();
    fork_writer_->CheckChild();
    if (was_in_progress && !fork_writer_->IsInProgress()) {
      CleanupOldSnapshots();
    }
  }

  utils::StructuredLog().Event("snapshot_scheduler_thread_exiting").Info();
}

void SnapshotScheduler::TakeSnapshot() {
  try {
    // Atomically try to acquire the read_only flag
    // This prevents TOCTOU race between checking and setting the flag
    bool expected = false;
    if (!read_only_.compare_exchange_strong(expected, true)) {
      // Another dump operation (manual or auto) is already in progress
      utils::StructuredLog()
          .Event("auto_snapshot_skipped")
          .Field("reason", "another DUMP operation is in progress")
          .Info();
      return;
    }

    // Flag successfully acquired, use RAII guard to ensure it's reset on exit
    utils::FlagResetGuard read_only_guard(read_only_);

    // Generate timestamp-based filename
    auto timestamp = std::time(nullptr);
    std::tm tm_buf{};
    localtime_r(&timestamp, &tm_buf);  // Thread-safe version of localtime
    std::ostringstream filename;
    filename << "auto_" << std::put_time(&tm_buf, "%Y%m%d_%H%M%S") << ".nvec";

    std::filesystem::path snapshot_path = std::filesystem::path(config_.dir) / filename.str();

    utils::StructuredLog().Event("snapshot_taking").Field("path", snapshot_path.string()).Info();

    // Start background fork-based snapshot
    auto result = fork_writer_->StartBackgroundSave(snapshot_path.string(), *full_config_, *event_store_, *co_index_,
                                                    *vector_store_, metadata_store_);

    if (result) {
      utils::StructuredLog().Event("snapshot_started").Field("path", snapshot_path.string()).Info();
    } else {
      utils::StructuredLog()
          .Event("server_error")
          .Field("operation", "snapshot_save")
          .Field("filepath", snapshot_path.string())
          .Field("error", result.error().message())
          .Error();
    }

  } catch (const std::exception& e) {
    utils::StructuredLog()
        .Event("server_error")
        .Field("operation", "snapshot_save")
        .Field("type", "exception")
        .Field("error", e.what())
        .Error();
  }
}

void ReclaimAbandonedTemporaries(const std::string& dir) {
  std::error_code error;
  const std::filesystem::path snapshot_dir(dir);
  if (!std::filesystem::is_directory(snapshot_dir, error)) {
    return;
  }
  for (const auto& entry : std::filesystem::directory_iterator(snapshot_dir, error)) {
    if (!entry.is_regular_file(error) || !IsAbandonedTemporary(entry.path().filename().string())) {
      continue;
    }
    utils::StructuredLog().Event("snapshot_removing_abandoned_temp").Field("path", entry.path().string()).Info();
    std::error_code remove_error;
    std::filesystem::remove(entry.path(), remove_error);
  }
}

void SnapshotScheduler::CleanupOldSnapshots() {
  // Reclaim before the retention check: temporaries are dot-prefixed and carry
  // the writing process id, so the retention scan never sees them, and a
  // `retain` of 0 would otherwise skip the sweep entirely and let a killed
  // writer's full-size file accumulate on every interrupted run.
  ReclaimAbandonedTemporaries(config_.dir);

  if (config_.retain <= 0) {
    return;
  }

  try {
    std::filesystem::path snapshot_dir(config_.dir);

    if (!std::filesystem::exists(snapshot_dir) || !std::filesystem::is_directory(snapshot_dir)) {
      return;
    }

    // Collect all auto_*.nvec files with their modification times
    std::vector<std::pair<std::filesystem::path, std::filesystem::file_time_type>> snapshot_files;

    for (const auto& entry : std::filesystem::directory_iterator(snapshot_dir)) {
      if (!entry.is_regular_file()) {
        continue;
      }
      const std::string name = entry.path().filename().string();
      if (entry.path().extension() == ".nvec") {
        // Only manage auto-saved files (starting with "auto_")
        if (name.rfind("auto_", 0) == 0) {
          snapshot_files.emplace_back(entry.path(), std::filesystem::last_write_time(entry));
        }
      }
    }

    // Sort by modification time (newest first)
    std::sort(snapshot_files.begin(), snapshot_files.end(),
              [](const auto& lhs, const auto& rhs) { return lhs.second > rhs.second; });

    // Delete old files beyond retain count
    const auto retain_count = static_cast<size_t>(config_.retain);
    for (size_t i = retain_count; i < snapshot_files.size(); ++i) {
      utils::StructuredLog().Event("snapshot_removing_old").Field("path", snapshot_files[i].first.string()).Info();
      std::filesystem::remove(snapshot_files[i].first);
    }

  } catch (const std::exception& e) {
    utils::StructuredLog()
        .Event("server_error")
        .Field("operation", "snapshot_cleanup")
        .Field("type", "exception")
        .Field("error", e.what())
        .Error();
  }
}

}  // namespace nvecd::server
