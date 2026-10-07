/**
 * @file snapshot_scheduler_test.cpp
 * @brief Unit tests for SnapshotScheduler
 */

#include "server/snapshot_scheduler.h"

#include <gtest/gtest.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "events/co_occurrence_index.h"
#include "events/event_store.h"
#include "storage/snapshot_fork.h"
#include "storage/wal_checkpoint.h"
#include "vectors/vector_store.h"

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
  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove_all(temp_dir_, ec);
  }

  static uint64_t GetCurrentTimestamp() {
    return static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
  }

  std::filesystem::path temp_dir_;
  std::atomic<bool> read_only_{false};
  std::atomic<bool> loading_{false};
  ServerStats stats_;

  config::EventsConfig events_cfg_;
  config::VectorsConfig vectors_cfg_;
  events::EventStore event_store_{events_cfg_};
  events::CoOccurrenceIndex co_index_;
  vectors::VectorStore vector_store_{vectors_cfg_};
  storage::ForkSnapshotWriter fork_writer_;
  config::Config full_config_;
  HandlerContext ctx_{&event_store_, &co_index_,    &vector_store_, nullptr,    nullptr, nullptr, nullptr,
                      stats_,        &full_config_, loading_,       read_only_, "",      ""};

  void SetUp() override {
    temp_dir_ = std::filesystem::temp_directory_path() / ("nvecd_sched_test_" + std::to_string(GetCurrentTimestamp()));
    std::filesystem::create_directories(temp_dir_);
    ctx_.fork_snapshot_writer = &fork_writer_;
  }
};

TEST_F(SnapshotSchedulerTest, DisabledWhenIntervalZero) {
  auto snap_config = MakeTestConfig(0, 3, temp_dir_.string());

  SnapshotScheduler scheduler(snap_config, ctx_);

  scheduler.Start();
  EXPECT_FALSE(scheduler.IsRunning());
}

TEST_F(SnapshotSchedulerTest, DisabledWhenIntervalNegative) {
  auto snap_config = MakeTestConfig(-1, 3, temp_dir_.string());

  SnapshotScheduler scheduler(snap_config, ctx_);

  scheduler.Start();
  EXPECT_FALSE(scheduler.IsRunning());
}

TEST_F(SnapshotSchedulerTest, StartsAndStops) {
  auto snap_config = MakeTestConfig(5, 3, temp_dir_.string());

  SnapshotScheduler scheduler(snap_config, ctx_);

  scheduler.Start();
  EXPECT_TRUE(scheduler.IsRunning());

  scheduler.Stop();
  EXPECT_FALSE(scheduler.IsRunning());
}

TEST_F(SnapshotSchedulerTest, StopIsIdempotent) {
  auto snap_config = MakeTestConfig(5, 3, temp_dir_.string());

  SnapshotScheduler scheduler(snap_config, ctx_);

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
    SnapshotScheduler scheduler(snap_config, ctx_);
    scheduler.Start();
    EXPECT_TRUE(scheduler.IsRunning());
    // Destructor should call Stop() and join the thread
  }
  // If we get here without hanging, the destructor properly stopped the thread
}

TEST_F(SnapshotSchedulerTest, CleanupKeepsExactlyTheNewestAutoSnapshotsAndTheirSidecars) {
  namespace fs = std::filesystem;
  const auto now = fs::file_time_type::clock::now();
  std::vector<fs::path> fakes;
  for (int i = 0; i < 5; ++i) {
    const fs::path path = temp_dir_ / ("auto_20260101_00000" + std::to_string(i) + ".nvec");
    std::ofstream(path) << "test data " << i;
    std::ofstream(path.string() + storage::kWalCheckpointSuffix) << "sidecar " << i;
    // Older index means older snapshot; every fake predates the real one.
    fs::last_write_time(path, now - std::chrono::minutes(10 - i));
    fakes.push_back(path);
  }
  std::ofstream(temp_dir_ / "manual_snapshot.nvec") << "manual data";

  const auto auto_snapshots = [this] {
    std::vector<std::string> names;
    for (const auto& entry : fs::directory_iterator(temp_dir_)) {
      const std::string name = entry.path().filename().string();
      if (entry.path().extension() == ".nvec" && name.rfind("auto_", 0) == 0) {
        names.push_back(name);
      }
    }
    std::sort(names.begin(), names.end());
    return names;
  };

  SnapshotScheduler scheduler(MakeTestConfig(1, 3, temp_dir_.string()), ctx_);
  scheduler.Start();
  ASSERT_TRUE(scheduler.IsRunning());

  // The first tick forks a real snapshot (six files). Once it is published,
  // a load-in-progress flag makes every later tick refuse to fork, so
  // retention runs exactly once, after that child is reaped.
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  while (auto_snapshots().size() != 6 && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  loading_.store(true);
  while (auto_snapshots().size() != 3 && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  scheduler.Stop();

  const auto remaining = auto_snapshots();
  ASSERT_EQ(remaining.size(), 3U);
  // Survivors are the real snapshot plus the two newest fakes.
  EXPECT_EQ(remaining[0], fakes[3].filename().string());
  EXPECT_EQ(remaining[1], fakes[4].filename().string());
  EXPECT_EQ(remaining[2].rfind("auto_2", 0), 0U);

  for (int i = 0; i < 5; ++i) {
    const bool kept = i >= 3;
    EXPECT_EQ(fs::exists(fakes[i]), kept) << fakes[i];
    EXPECT_EQ(fs::exists(fakes[i].string() + storage::kWalCheckpointSuffix), kept) << fakes[i];
  }
  EXPECT_TRUE(fs::exists(temp_dir_ / "manual_snapshot.nvec"));
}

TEST_F(SnapshotSchedulerTest, CleanupReclaimsTemporariesOfDeadWritersOnly) {
  // Snapshot temporaries are named ".<snapshot>.tmp.<pid>.<n>", so the retention
  // scan (which matches auto_*.nvec) never sees them. A writer killed mid-write
  // leaves a full-size file behind, and without this sweep the directory grows
  // without bound no matter what `retain` says.
  const std::filesystem::path abandoned = temp_dir_ / ".auto_20260101_000000.nvec.tmp.999999999.0";
  const std::filesystem::path live =
      temp_dir_ / (".auto_20260101_000001.nvec.tmp." + std::to_string(::getpid()) + ".0");
  for (const auto& path : {abandoned, live}) {
    std::ofstream ofs(path);
    ofs << "partial snapshot";
  }
  ASSERT_TRUE(std::filesystem::exists(abandoned));
  ASSERT_TRUE(std::filesystem::exists(live));

  auto snap_config = MakeTestConfig(1, 3, temp_dir_.string());
  SnapshotScheduler scheduler(snap_config, ctx_);
  scheduler.Start();
  ASSERT_TRUE(scheduler.IsRunning());
  std::this_thread::sleep_for(std::chrono::milliseconds(2500));
  scheduler.Stop();

  EXPECT_FALSE(std::filesystem::exists(abandoned));
  // A temporary owned by a living process is still being written; removing it
  // would corrupt an in-flight snapshot.
  EXPECT_TRUE(std::filesystem::exists(live));
}

}  // namespace
}  // namespace nvecd::server
