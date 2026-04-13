/**
 * @file snapshot_scheduler.cpp
 * @brief Implementation of SnapshotScheduler
 *
 * Reference: ../mygram-db/src/server/snapshot_scheduler.cpp
 * Reusability: 75% (removed MySQL/GTID/TableCatalog, uses ForkSnapshotWriter)
 */

#include "server/snapshot_scheduler.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <sstream>

#include "utils/flag_guard.h"
#include "utils/structured_log.h"

namespace nvecd::server {

namespace {
constexpr int kCheckIntervalMs = 1000;  // Check for shutdown every second
}  // namespace

SnapshotScheduler::SnapshotScheduler(config::SnapshotConfig config, storage::ForkSnapshotWriter* fork_writer,
                                     const config::Config* full_config, events::EventStore* event_store,
                                     events::CoOccurrenceIndex* co_index, vectors::VectorStore* vector_store,
                                     std::atomic<bool>& read_only)
    : config_(std::move(config)),
      fork_writer_(fork_writer),
      full_config_(full_config),
      event_store_(event_store),
      co_index_(co_index),
      vector_store_(vector_store),
      read_only_(read_only) {}

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
    auto now = std::chrono::steady_clock::now();

    // Check if it's time to save
    if (now >= next_save_time) {
      TakeSnapshot();
      CleanupOldSnapshots();

      // Schedule next save
      next_save_time = std::chrono::steady_clock::now() + std::chrono::seconds(interval_sec);
    }

    // Sleep for check interval
    std::this_thread::sleep_for(std::chrono::milliseconds(kCheckIntervalMs));
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
                                                    *vector_store_);

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

void SnapshotScheduler::CleanupOldSnapshots() {
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
      if (entry.is_regular_file() && entry.path().extension() == ".nvec") {
        // Only manage auto-saved files (starting with "auto_")
        if (entry.path().filename().string().rfind("auto_", 0) == 0) {
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
