/**
 * @file snapshot_session_test.cpp
 * @brief Tests that pin the snapshot durability handshake to one behaviour
 *
 * The handshake — collect the writer's result, write the checkpoint sidecar,
 * truncate the WAL, report the outcome — must be identical whichever snapshot
 * mode produced the file. These tests drive both modes over the same inputs
 * and compare the resulting side-effect sets, so a change that reaches only
 * one mode fails here.
 */

#include "storage/snapshot_session.h"

#include <gtest/gtest.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "config/config.h"
#include "events/co_occurrence_index.h"
#include "events/event_store.h"
#include "storage/snapshot_fork.h"
#include "storage/snapshot_format_v1.h"
#include "storage/snapshot_lock.h"
#include "storage/wal.h"
#include "storage/wal_checkpoint.h"
#include "vectors/vector_store.h"

using namespace nvecd;

namespace {

/// Everything an operator can observe after a snapshot attempt settles.
struct SideEffects {
  bool succeeded = false;
  bool snapshot_exists = false;
  bool sidecar_exists = false;
  uint64_t sidecar_sequence = 0;
  uint64_t wal_sequence = 0;
  std::vector<std::string> wal_segments;
  size_t restored_vector_count = 0;

  bool operator==(const SideEffects& other) const {
    return succeeded == other.succeeded && snapshot_exists == other.snapshot_exists &&
           sidecar_exists == other.sidecar_exists && sidecar_sequence == other.sidecar_sequence &&
           wal_sequence == other.wal_sequence && wal_segments == other.wal_segments &&
           restored_vector_count == other.restored_vector_count;
  }
};

std::ostream& operator<<(std::ostream& out, const SideEffects& effects) {
  out << "{succeeded=" << effects.succeeded << ", snapshot=" << effects.snapshot_exists
      << ", sidecar=" << effects.sidecar_exists << ", sidecar_sequence=" << effects.sidecar_sequence
      << ", wal_sequence=" << effects.wal_sequence << ", segments=[";
  for (const auto& segment : effects.wal_segments) {
    out << segment << " ";
  }
  return out << "], vectors=" << effects.restored_vector_count << "}";
}

std::vector<std::string> ListWalSegments(const std::filesystem::path& wal_dir) {
  std::vector<std::string> segments;
  for (const auto& entry : std::filesystem::directory_iterator(wal_dir)) {
    if (entry.path().extension() == ".log") {
      segments.push_back(entry.path().filename().string());
    }
  }
  std::sort(segments.begin(), segments.end());
  return segments;
}

}  // namespace

/// Stores, a WAL with several segments, and one snapshot path per mode.
class SnapshotSessionTest : public ::testing::Test {
 protected:
  void SetUp() override {
    config_.events.ctx_buffer_size = 50;
    config_.vectors.default_dimension = 3;
    config_.vectors.distance_metric = "cosine";

    root_dir_ = std::filesystem::temp_directory_path() / ("nvecd_session_test_" + std::to_string(::getpid()));
    std::filesystem::remove_all(root_dir_);
    std::filesystem::create_directories(root_dir_);
    std::filesystem::permissions(root_dir_, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace);
  }

  void TearDown() override { std::filesystem::remove_all(root_dir_); }

  /// A private directory for one mode's snapshot and WAL.
  std::filesystem::path MakeModeDir(const std::string& name) {
    const auto dir = root_dir_ / name;
    std::filesystem::create_directories(dir / "wal");
    std::filesystem::permissions(dir, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace);
    std::filesystem::permissions(dir / "wal", std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::replace);
    return dir;
  }

  /// Identical starting state for every mode: two vectors, two events.
  void PopulateStores(events::EventStore& event_store, events::CoOccurrenceIndex& co_index,
                      vectors::VectorStore& vector_store) const {
    event_store.AddEvent("ctx1", "item1", 10);
    event_store.AddEvent("ctx1", "item2", 20);
    vector_store.SetVector("item1", {1.0F, 0.0F, 0.0F});
    vector_store.SetVector("item2", {0.0F, 1.0F, 0.0F});
  }

  /// Open a WAL with small segments and fill it so truncation is observable.
  void OpenAndFillWal(storage::WriteAheadLog& wal, const std::filesystem::path& wal_dir) const {
    storage::WriteAheadLog::Config wal_config;
    wal_config.directory = wal_dir.string();
    wal_config.max_file_size = 256;
    wal_config.sync_on_write = true;
    ASSERT_TRUE(wal.Open(wal_config).has_value());
    for (int index = 0; index < 20; ++index) {
      const std::string payload = "record_" + std::to_string(index);
      ASSERT_TRUE(
          wal.Append(storage::WalOpType::kVecSet, payload.data(), static_cast<uint32_t>(payload.size())).has_value());
    }
  }

  SideEffects Observe(bool succeeded, const std::string& snapshot_path, const std::filesystem::path& wal_dir) const {
    SideEffects effects;
    effects.succeeded = succeeded;
    effects.snapshot_exists = std::filesystem::exists(snapshot_path);
    auto sidecar = storage::ReadWalCheckpoint(snapshot_path);
    effects.sidecar_exists = sidecar.has_value();
    effects.sidecar_sequence = sidecar.has_value() ? *sidecar : 0;
    effects.wal_segments = ListWalSegments(wal_dir);
    if (effects.snapshot_exists) {
      config::Config loaded_config;
      events::EventStore loaded_es(config_.events);
      events::CoOccurrenceIndex loaded_co;
      vectors::VectorStore loaded_vs(config_.vectors);
      if (storage::snapshot_v1::ReadSnapshotV1(snapshot_path, loaded_config, loaded_es, loaded_co, loaded_vs)) {
        effects.restored_vector_count = loaded_vs.GetVectorCount();
      }
    }
    return effects;
  }

  /// Drive lock mode end to end and report what it left behind.
  SideEffects RunLockMode(bool block_sidecar) {
    const auto dir = MakeModeDir("lock");
    const std::string snapshot_path = (dir / "snapshot.nvec").string();
    events::EventStore event_store(config_.events);
    events::CoOccurrenceIndex co_index;
    vectors::VectorStore vector_store(config_.vectors);
    PopulateStores(event_store, co_index, vector_store);

    storage::WriteAheadLog wal;
    OpenAndFillWal(wal, dir / "wal");
    if (block_sidecar) {
      std::filesystem::create_directories(snapshot_path + storage::kWalCheckpointSuffix);
    }

    uint64_t captured = 0;
    auto write = storage::WriteSnapshotWithLock(snapshot_path, config_, event_store, co_index, vector_store, nullptr,
                                                nullptr, nullptr, &wal, &captured);
    storage::SnapshotSession session(snapshot_path, std::move(write), captured, &wal);
    auto outcome = session.Finish(0);

    SideEffects effects = Observe(outcome.has_value(), snapshot_path, dir / "wal");
    effects.wal_sequence = wal.CurrentSequence();
    wal.Close();
    return effects;
  }

  /// Drive fork mode end to end and report what it left behind.
  SideEffects RunForkMode(bool block_sidecar) {
    const auto dir = MakeModeDir("fork");
    const std::string snapshot_path = (dir / "snapshot.nvec").string();
    events::EventStore event_store(config_.events);
    events::CoOccurrenceIndex co_index;
    vectors::VectorStore vector_store(config_.vectors);
    PopulateStores(event_store, co_index, vector_store);

    storage::WriteAheadLog wal;
    OpenAndFillWal(wal, dir / "wal");
    if (block_sidecar) {
      std::filesystem::create_directories(snapshot_path + storage::kWalCheckpointSuffix);
    }

    bool succeeded = false;
    {
      storage::ForkSnapshotWriter writer;
      writer.SetWal(&wal);
      EXPECT_TRUE(writer.StartBackgroundSave(snapshot_path, config_, event_store, co_index, vector_store).has_value());
      writer.WaitForChild(10000);
      succeeded = writer.GetStatus().status == storage::SnapshotStatus::kCompleted;
    }

    SideEffects effects = Observe(succeeded, snapshot_path, dir / "wal");
    effects.wal_sequence = wal.CurrentSequence();
    wal.Close();
    return effects;
  }

  config::Config config_;
  std::filesystem::path root_dir_;
};

TEST_F(SnapshotSessionTest, BothModesProduceTheSameSideEffectsOnSuccess) {
  const SideEffects lock_mode = RunLockMode(/*block_sidecar=*/false);
  const SideEffects fork_mode = RunForkMode(/*block_sidecar=*/false);

  EXPECT_TRUE(lock_mode.succeeded);
  EXPECT_TRUE(lock_mode.snapshot_exists);
  EXPECT_TRUE(lock_mode.sidecar_exists);
  EXPECT_EQ(lock_mode.sidecar_sequence, lock_mode.wal_sequence);
  EXPECT_EQ(lock_mode.wal_segments.size(), 1U) << "truncation should retain only the active segment";
  EXPECT_EQ(lock_mode.restored_vector_count, 2U);
  EXPECT_EQ(lock_mode, fork_mode) << "lock=" << lock_mode << " fork=" << fork_mode;
}

TEST_F(SnapshotSessionTest, BothModesFailTheSameWayWhenTheSidecarCannotBeWritten) {
  const SideEffects lock_mode = RunLockMode(/*block_sidecar=*/true);
  const SideEffects fork_mode = RunForkMode(/*block_sidecar=*/true);

  // A snapshot whose checkpoint is not durable is a failed snapshot in both
  // modes, and neither may truncate the WAL it could not bind itself to.
  EXPECT_FALSE(lock_mode.succeeded);
  EXPECT_FALSE(lock_mode.sidecar_exists);
  EXPECT_GT(lock_mode.wal_segments.size(), 1U);
  EXPECT_EQ(lock_mode, fork_mode) << "lock=" << lock_mode << " fork=" << fork_mode;
}

TEST_F(SnapshotSessionTest, ForkModeCompletesTheHandshakeWithoutBeingPolled) {
  // No scheduler, no DUMP STATUS, no shutdown: the snapshot must still bind
  // its checkpoint and truncate the WAL on its own.
  const auto dir = MakeModeDir("unpolled");
  const std::string snapshot_path = (dir / "snapshot.nvec").string();
  events::EventStore event_store(config_.events);
  events::CoOccurrenceIndex co_index;
  vectors::VectorStore vector_store(config_.vectors);
  PopulateStores(event_store, co_index, vector_store);

  storage::WriteAheadLog wal;
  OpenAndFillWal(wal, dir / "wal");
  ASSERT_GT(ListWalSegments(dir / "wal").size(), 1U);

  storage::ForkSnapshotWriter writer;
  writer.SetWal(&wal);
  ASSERT_TRUE(writer.StartBackgroundSave(snapshot_path, config_, event_store, co_index, vector_store).has_value());

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (std::chrono::steady_clock::now() < deadline && ListWalSegments(dir / "wal").size() > 1U) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }

  EXPECT_TRUE(storage::ReadWalCheckpoint(snapshot_path).has_value());
  EXPECT_EQ(ListWalSegments(dir / "wal").size(), 1U);
  wal.Close();
}

TEST_F(SnapshotSessionTest, DroppingAStartedSessionStillCompletesTheHandshake) {
  const auto dir = MakeModeDir("dropped");
  const std::string snapshot_path = (dir / "snapshot.nvec").string();
  events::EventStore event_store(config_.events);
  events::CoOccurrenceIndex co_index;
  vectors::VectorStore vector_store(config_.vectors);
  PopulateStores(event_store, co_index, vector_store);

  storage::WriteAheadLog wal;
  OpenAndFillWal(wal, dir / "wal");

  uint64_t captured = 0;
  auto write = storage::WriteSnapshotWithLock(snapshot_path, config_, event_store, co_index, vector_store, nullptr,
                                              nullptr, nullptr, &wal, &captured);
  ASSERT_TRUE(write.has_value()) << write.error().message();
  {
    // The owner never calls Finish(); the destructor must run the handshake.
    storage::SnapshotSession session(snapshot_path, std::move(write), captured, &wal);
  }

  auto sidecar = storage::ReadWalCheckpoint(snapshot_path);
  ASSERT_TRUE(sidecar.has_value()) << sidecar.error().message();
  EXPECT_EQ(*sidecar, captured);
  EXPECT_EQ(ListWalSegments(dir / "wal").size(), 1U);
  wal.Close();
}

TEST_F(SnapshotSessionTest, ConcurrentStatusChecksNeverDowngradeACompletedSnapshot) {
  const auto dir = MakeModeDir("concurrent");
  const std::string snapshot_path = (dir / "snapshot.nvec").string();
  events::EventStore event_store(config_.events);
  events::CoOccurrenceIndex co_index;
  vectors::VectorStore vector_store(config_.vectors);
  PopulateStores(event_store, co_index, vector_store);

  storage::ForkSnapshotWriter writer;
  ASSERT_TRUE(writer.StartBackgroundSave(snapshot_path, config_, event_store, co_index, vector_store).has_value());

  // Every caller that polls the child races over the same exit status. Only
  // one reap may happen, so no poller can observe a failure for a child that
  // wrote its snapshot successfully.
  constexpr size_t kPollers = 8;
  std::atomic<bool> observed_failure{false};
  std::atomic<bool> stop{false};
  std::vector<std::thread> pollers;
  pollers.reserve(kPollers);
  for (size_t index = 0; index < kPollers; ++index) {
    pollers.emplace_back([&] {
      while (!stop.load(std::memory_order_acquire)) {
        writer.CheckChild();
        if (writer.GetStatus().status == storage::SnapshotStatus::kFailed) {
          observed_failure.store(true, std::memory_order_release);
        }
      }
    });
  }

  writer.WaitForChild(10000);
  stop.store(true, std::memory_order_release);
  for (auto& poller : pollers) {
    poller.join();
  }

  EXPECT_FALSE(observed_failure.load(std::memory_order_acquire));
  EXPECT_EQ(writer.GetStatus().status, storage::SnapshotStatus::kCompleted) << writer.GetStatus().error_message;
}

TEST_F(SnapshotSessionTest, ForkSnapshotCapturesTheStateAtForkTime) {
  const auto dir = MakeModeDir("frozen");
  const std::string snapshot_path = (dir / "snapshot.nvec").string();
  events::EventStore event_store(config_.events);
  events::CoOccurrenceIndex co_index;
  vectors::VectorStore vector_store(config_.vectors);
  PopulateStores(event_store, co_index, vector_store);

  storage::ForkSnapshotWriter writer;
  ASSERT_TRUE(writer.StartBackgroundSave(snapshot_path, config_, event_store, co_index, vector_store).has_value());

  // Mutations after the fork must not appear in the child's frozen image.
  vector_store.SetVector("item3", {0.0F, 0.0F, 1.0F});
  event_store.AddEvent("ctx1", "item3", 30);

  writer.WaitForChild(10000);
  ASSERT_EQ(writer.GetStatus().status, storage::SnapshotStatus::kCompleted) << writer.GetStatus().error_message;

  config::Config loaded_config;
  events::EventStore loaded_es(config_.events);
  events::CoOccurrenceIndex loaded_co;
  vectors::VectorStore loaded_vs(config_.vectors);
  auto read = storage::snapshot_v1::ReadSnapshotV1(snapshot_path, loaded_config, loaded_es, loaded_co, loaded_vs);
  ASSERT_TRUE(read.has_value()) << read.error().message();

  EXPECT_EQ(loaded_vs.GetVectorCount(), 2U);
  EXPECT_FALSE(loaded_vs.GetVector("item3").has_value());
}

TEST_F(SnapshotSessionTest, ForkSnapshotsCompleteWhileWritersContendForTheStores) {
  // The parent acquires the store barrier, logs, and flushes the logger before
  // it forks. A writer arriving anywhere in that window leaves "writer waiting"
  // set in the copy the child inherits, and the child's serializer then blocks
  // on a thread that does not exist there. One such hang would strand the
  // snapshot in progress for the life of the process.
  const auto dir = MakeModeDir("contended");
  events::EventStore event_store(config_.events);
  events::CoOccurrenceIndex co_index;
  vectors::VectorStore vector_store(config_.vectors);
  PopulateStores(event_store, co_index, vector_store);

  std::atomic<bool> stop{false};
  std::vector<std::thread> writers;
  writers.reserve(3);
  for (size_t index = 0; index < 3; ++index) {
    writers.emplace_back([&, index] {
      size_t counter = 0;
      while (!stop.load(std::memory_order_acquire)) {
        const std::string id = "w" + std::to_string(index) + "_" + std::to_string(counter++);
        vector_store.SetVector(id, {1.0F, 0.0F, 0.0F});
        event_store.AddEvent("ctx_contended", id, 5);
        vector_store.DeleteVector(id);
      }
    });
  }

  constexpr size_t kSnapshots = 12;
  size_t completed = 0;
  for (size_t index = 0; index < kSnapshots; ++index) {
    storage::ForkSnapshotWriter writer;
    const std::string path = (dir / ("contended-" + std::to_string(index) + ".nvec")).string();
    ASSERT_TRUE(writer.StartBackgroundSave(path, config_, event_store, co_index, vector_store).has_value());
    writer.WaitForChild(10000);
    if (writer.GetStatus().status == storage::SnapshotStatus::kCompleted) {
      ++completed;
    }
  }

  stop.store(true, std::memory_order_release);
  for (auto& thread : writers) {
    thread.join();
  }
  EXPECT_EQ(completed, kSnapshots);
}

TEST_F(SnapshotSessionTest, StoppingAWriterMidFlightLeavesNoTemporaryBehind) {
  // A forced shutdown signals the child while it is still writing. Whoever
  // wins the race, the directory must end up holding only finished artefacts:
  // a snapshot temporary is dot-prefixed and invisible to retention, so one
  // left behind is never reclaimed.
  const auto dir = MakeModeDir("interrupted");
  const std::string snapshot_path = (dir / "snapshot.nvec").string();
  events::EventStore event_store(config_.events);
  events::CoOccurrenceIndex co_index;
  vectors::VectorStore vector_store(config_.vectors);
  for (int index = 0; index < 20000; ++index) {
    const std::string id = "bulk_" + std::to_string(index);
    vector_store.SetVector(id, {static_cast<float>(index), 1.0F, 0.0F});
  }

  storage::SnapshotStatus status = storage::SnapshotStatus::kIdle;
  {
    storage::ForkSnapshotWriter writer;
    ASSERT_TRUE(writer.StartBackgroundSave(snapshot_path, config_, event_store, co_index, vector_store).has_value());
    writer.WaitForChild(0);  // no grace period: signal the child immediately
    status = writer.GetStatus().status;
  }

  EXPECT_NE(status, storage::SnapshotStatus::kInProgress);
  for (const auto& entry : std::filesystem::directory_iterator(dir)) {
    EXPECT_EQ(entry.path().filename().string().find(".tmp."), std::string::npos)
        << "left behind: " << entry.path().string();
  }
}

TEST_F(SnapshotSessionTest, SecondSnapshotStartsWithoutAnyExplicitReap) {
  // A caller that never polls for status must still be able to take the next
  // snapshot: the previous session is released when a new one is requested.
  const auto dir = MakeModeDir("sequential");
  events::EventStore event_store(config_.events);
  events::CoOccurrenceIndex co_index;
  vectors::VectorStore vector_store(config_.vectors);
  PopulateStores(event_store, co_index, vector_store);

  storage::ForkSnapshotWriter writer;
  const std::string first = (dir / "first.nvec").string();
  ASSERT_TRUE(writer.StartBackgroundSave(first, config_, event_store, co_index, vector_store).has_value());
  writer.WaitForChild(10000);

  const std::string second = (dir / "second.nvec").string();
  auto started = writer.StartBackgroundSave(second, config_, event_store, co_index, vector_store);
  ASSERT_TRUE(started.has_value()) << started.error().message();
  writer.WaitForChild(10000);
  EXPECT_EQ(writer.GetStatus().status, storage::SnapshotStatus::kCompleted) << writer.GetStatus().error_message;
  EXPECT_TRUE(std::filesystem::exists(second));
}
