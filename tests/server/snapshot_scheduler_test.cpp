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
#include "server/handlers/dump_handler.h"
#include "storage/snapshot_fork.h"
#include "storage/snapshot_format_v1.h"
#include "storage/wal.h"
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

  // The interval never elapses, so the only child is the one started here and
  // retention runs exactly once, after the scheduler reaps it.
  SnapshotScheduler scheduler(MakeTestConfig(3600, 3, temp_dir_.string()), ctx_);
  scheduler.Start();
  ASSERT_TRUE(scheduler.IsRunning());
  ASSERT_TRUE(handlers::StartForkSnapshot(ctx_, (temp_dir_ / "auto_20990101_000000.nvec").string()));

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
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

TEST_F(SnapshotSchedulerTest, StartupSweepReclaimsTemporariesCarryingTheOwnPid) {
  // Before this process starts a writer, a temporary with its PID was left by
  // a previous run that had the same PID (PID 1 in a container).
  const std::filesystem::path own = temp_dir_ / (".auto_20260101_000000.nvec.tmp." + std::to_string(::getpid()) + ".0");
  std::ofstream(own) << "partial snapshot";

  ReclaimAbandonedTemporaries(temp_dir_.string(), /*own_pid_is_abandoned=*/false);
  EXPECT_TRUE(std::filesystem::exists(own));
  ReclaimAbandonedTemporaries(temp_dir_.string(), /*own_pid_is_abandoned=*/true);
  EXPECT_FALSE(std::filesystem::exists(own));
}

TEST_F(SnapshotSchedulerTest, RefusedScheduledSnapshotIsRetriedOnTheNextTick) {
  const auto has_auto_snapshot = [this] {
    for (const auto& entry : std::filesystem::directory_iterator(temp_dir_)) {
      if (entry.path().filename().string().rfind("auto_", 0) == 0) {
        return true;
      }
    }
    return false;
  };

  // The snapshot due at 2 s is refused by a load; with the load gone the 3 s
  // tick must take it, not the 4 s tick a full interval later.
  loading_.store(true);
  const auto started = std::chrono::steady_clock::now();
  SnapshotScheduler scheduler(MakeTestConfig(2, 3, temp_dir_.string()), ctx_);
  scheduler.Start();
  std::this_thread::sleep_until(started + std::chrono::milliseconds(2400));
  EXPECT_FALSE(has_auto_snapshot());
  loading_.store(false);

  while (!has_auto_snapshot() && std::chrono::steady_clock::now() < started + std::chrono::milliseconds(3700)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  const bool retried = has_auto_snapshot();
  scheduler.Stop();
  EXPECT_TRUE(retried) << "the refused snapshot waited for the next interval";
}

TEST_F(SnapshotSchedulerTest, RetentionWithWalCountsOnlySnapshotsWhoseSidecarValidates) {
  namespace fs = std::filesystem;
  storage::WriteAheadLog wal;
  storage::WriteAheadLog::Config wal_config;
  wal_config.directory = (temp_dir_ / "wal").string();
  fs::create_directories(wal_config.directory);
  ASSERT_TRUE(wal.Open(wal_config));
  ASSERT_TRUE(wal.Append(storage::WalOpType::kVecSet, nullptr, 0));
  ctx_.wal = &wal;

  // The base the WAL was last truncated against, older than everything else.
  const auto now = fs::file_time_type::clock::now();
  const fs::path base = temp_dir_ / "auto_20260101_000000.nvec";
  ASSERT_TRUE(
      storage::snapshot_v1::WriteSnapshotV1(base.string(), full_config_, event_store_, co_index_, vector_store_));
  ASSERT_TRUE(storage::WriteWalCheckpoint(base.string(), wal.CurrentSequence()));
  fs::last_write_time(base, now - std::chrono::minutes(10));
  // A newer snapshot whose sidecar write failed.
  const fs::path unbound = temp_dir_ / "auto_20260101_000001.nvec";
  std::ofstream(unbound) << "snapshot without sidecar";
  fs::last_write_time(unbound, now - std::chrono::minutes(5));

  // The fork writer has no WAL, so the child it reaps publishes no sidecar
  // either: the newest file is again one whose sidecar never landed.
  const fs::path newest = temp_dir_ / "auto_20990101_000000.nvec";
  SnapshotScheduler scheduler(MakeTestConfig(3600, 1, temp_dir_.string()), ctx_);
  scheduler.Start();
  ASSERT_TRUE(handlers::StartForkSnapshot(ctx_, newest.string()));
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
  while (fs::exists(unbound) && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  scheduler.Stop();
  ctx_.wal = nullptr;

  EXPECT_FALSE(fs::exists(unbound));
  EXPECT_FALSE(fs::exists(newest));
  ASSERT_TRUE(fs::exists(base));
  EXPECT_TRUE(storage::ReadWalCheckpoint(base.string()));
  const auto candidates = storage::ListRecoveryCandidates(temp_dir_.string(), /*require_checkpoint=*/true);
  ASSERT_EQ(candidates.size(), 1U);
  EXPECT_EQ(fs::path(candidates.front()).filename().string(), base.filename().string());
}

}  // namespace
}  // namespace nvecd::server
