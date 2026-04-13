/**
 * @file snapshot_scheduler_test.cpp
 * @brief Unit tests for SnapshotScheduler
 */

#include "server/snapshot_scheduler.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

namespace nvecd::server {
namespace {

/// Helper to create a minimal config for testing
config::SnapshotConfig MakeTestConfig(int interval_sec, int retain, const std::string& dir) {
  config::SnapshotConfig cfg;
  cfg.interval_sec = interval_sec;
  cfg.retain = retain;
  cfg.dir = dir;
  cfg.mode = "fork";
  return cfg;
}

/// Test fixture that provides common test infrastructure
class SnapshotSchedulerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // Create a unique temp directory for each test
    temp_dir_ = std::filesystem::temp_directory_path() / ("nvecd_sched_test_" + std::to_string(GetCurrentTimestamp()));
    std::filesystem::create_directories(temp_dir_);
  }

  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove_all(temp_dir_, ec);
  }

  static uint64_t GetCurrentTimestamp() {
    return static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
  }

  std::filesystem::path temp_dir_;
  std::atomic<bool> read_only_{false};

  // Minimal stores for constructor (scheduler won't actually use them
  // in disabled/start-stop tests)
  config::EventsConfig events_cfg_;
  config::VectorsConfig vectors_cfg_;
  events::EventStore event_store_{events_cfg_};
  events::CoOccurrenceIndex co_index_;
  vectors::VectorStore vector_store_{vectors_cfg_};
  storage::ForkSnapshotWriter fork_writer_;
  config::Config full_config_;
};

TEST_F(SnapshotSchedulerTest, DisabledWhenIntervalZero) {
  auto snap_config = MakeTestConfig(0, 3, temp_dir_.string());

  SnapshotScheduler scheduler(snap_config, &fork_writer_, &full_config_, &event_store_, &co_index_, &vector_store_,
                              read_only_);

  scheduler.Start();
  EXPECT_FALSE(scheduler.IsRunning());
}

TEST_F(SnapshotSchedulerTest, DisabledWhenIntervalNegative) {
  auto snap_config = MakeTestConfig(-1, 3, temp_dir_.string());

  SnapshotScheduler scheduler(snap_config, &fork_writer_, &full_config_, &event_store_, &co_index_, &vector_store_,
                              read_only_);

  scheduler.Start();
  EXPECT_FALSE(scheduler.IsRunning());
}

TEST_F(SnapshotSchedulerTest, StartsAndStops) {
  auto snap_config = MakeTestConfig(5, 3, temp_dir_.string());

  SnapshotScheduler scheduler(snap_config, &fork_writer_, &full_config_, &event_store_, &co_index_, &vector_store_,
                              read_only_);

  scheduler.Start();
  EXPECT_TRUE(scheduler.IsRunning());

  scheduler.Stop();
  EXPECT_FALSE(scheduler.IsRunning());
}

TEST_F(SnapshotSchedulerTest, StopIsIdempotent) {
  auto snap_config = MakeTestConfig(5, 3, temp_dir_.string());

  SnapshotScheduler scheduler(snap_config, &fork_writer_, &full_config_, &event_store_, &co_index_, &vector_store_,
                              read_only_);

  scheduler.Start();
  EXPECT_TRUE(scheduler.IsRunning());

  scheduler.Stop();
  EXPECT_FALSE(scheduler.IsRunning());

  // Second stop should be safe
  scheduler.Stop();
  EXPECT_FALSE(scheduler.IsRunning());
}

TEST_F(SnapshotSchedulerTest, DestructorStopsScheduler) {
  auto snap_config = MakeTestConfig(5, 3, temp_dir_.string());

  {
    SnapshotScheduler scheduler(snap_config, &fork_writer_, &full_config_, &event_store_, &co_index_, &vector_store_,
                                read_only_);
    scheduler.Start();
    EXPECT_TRUE(scheduler.IsRunning());
    // Destructor should call Stop() and join the thread
  }
  // If we get here without hanging, the destructor properly stopped the thread
}

TEST_F(SnapshotSchedulerTest, CleanupRetainsCorrectCount) {
  // Create 5 auto_*.nvec files in the temp directory with staggered modification times
  for (int i = 0; i < 5; ++i) {
    std::string filename = "auto_20260101_00000" + std::to_string(i) + ".nvec";
    std::filesystem::path filepath = temp_dir_ / filename;
    std::ofstream ofs(filepath);
    ofs << "test data " << i;
    ofs.close();

    // Stagger modification times so sorting is deterministic
    // Use resize to touch the file at slightly different times
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }

  // Also create a non-auto file that should NOT be cleaned up
  {
    std::filesystem::path manual_file = temp_dir_ / "manual_snapshot.nvec";
    std::ofstream ofs(manual_file);
    ofs << "manual data";
  }

  // Verify we have 5 auto files + 1 manual file
  int auto_count = 0;
  int total_count = 0;
  for (const auto& entry : std::filesystem::directory_iterator(temp_dir_)) {
    if (entry.is_regular_file() && entry.path().extension() == ".nvec") {
      ++total_count;
      if (entry.path().filename().string().rfind("auto_", 0) == 0) {
        ++auto_count;
      }
    }
  }
  ASSERT_EQ(auto_count, 5);
  ASSERT_EQ(total_count, 6);

  // Create scheduler with retain=3 and a very long interval (won't actually trigger)
  auto snap_config = MakeTestConfig(99999, 3, temp_dir_.string());

  SnapshotScheduler scheduler(snap_config, &fork_writer_, &full_config_, &event_store_, &co_index_, &vector_store_,
                              read_only_);

  // Start and immediately stop -- the first snapshot won't fire for 99999 seconds,
  // but we want the scheduler infrastructure for the test.
  // Instead, we'll use a short interval and wait for one cycle.
  // Actually, we can test cleanup indirectly by using a very short interval.

  // Better approach: use interval=1 so the scheduler fires quickly
  auto short_config = MakeTestConfig(1, 3, temp_dir_.string());

  SnapshotScheduler short_scheduler(short_config, &fork_writer_, &full_config_, &event_store_, &co_index_,
                                    &vector_store_, read_only_);

  short_scheduler.Start();
  ASSERT_TRUE(short_scheduler.IsRunning());

  // Wait for the scheduler to fire at least once (1s interval + some margin)
  std::this_thread::sleep_for(std::chrono::milliseconds(2500));

  short_scheduler.Stop();

  // Count remaining auto_*.nvec files
  auto_count = 0;
  int manual_count = 0;
  for (const auto& entry : std::filesystem::directory_iterator(temp_dir_)) {
    if (entry.is_regular_file() && entry.path().extension() == ".nvec") {
      if (entry.path().filename().string().rfind("auto_", 0) == 0) {
        ++auto_count;
      } else {
        ++manual_count;
      }
    }
  }

  // Should retain at most 3 auto files (the 3 newest).
  // The scheduler may have created additional auto files via TakeSnapshot(),
  // but cleanup should keep only 3.
  EXPECT_LE(auto_count, 3);

  // Manual file should be untouched
  EXPECT_EQ(manual_count, 1);
  EXPECT_TRUE(std::filesystem::exists(temp_dir_ / "manual_snapshot.nvec"));
}

}  // namespace
}  // namespace nvecd::server
