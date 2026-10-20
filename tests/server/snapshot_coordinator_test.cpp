/**
 * @file snapshot_coordinator_test.cpp
 * @brief Snapshot session admission, request exclusion and shutdown refusal
 */

#include "server/snapshot_coordinator.h"

#include <gtest/gtest.h>
#include <signal.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <future>
#include <string>
#include <thread>
#include <vector>

#include "config/config.h"
#include "events/co_occurrence_index.h"
#include "events/event_store.h"
#include "server/handlers/dump_handler.h"
#include "server/server_types.h"
#include "storage/snapshot_fork.h"
#include "storage/wal.h"
#include "storage/wal_checkpoint.h"
#include "vectors/vector_store.h"

namespace nvecd::server {
namespace {

using utils::ErrorCode;

constexpr auto kBlocked = std::chrono::milliseconds(50);
constexpr auto kPrompt = std::chrono::seconds(5);

class SnapshotCoordinatorTest : public ::testing::Test {
 protected:
  std::atomic<bool> loading_{false};
  std::atomic<bool> read_only_{false};
  SnapshotCoordinator coordinator_{loading_, read_only_};
};

TEST_F(SnapshotCoordinatorTest, ALoadRefusesEveryOtherSessionWithoutTouchingItsFlag) {
  auto load = coordinator_.BeginLoad(nullptr);
  ASSERT_TRUE(load.has_value()) << load.error().message();
  EXPECT_TRUE(loading_.load());

  auto save = coordinator_.BeginLockSave(nullptr);
  ASSERT_FALSE(save.has_value());
  EXPECT_EQ(save.error().code(), ErrorCode::kSnapshotAlreadyInProgress);
  EXPECT_FALSE(read_only_.load());

  auto brief = coordinator_.BeginBriefSession();
  ASSERT_FALSE(brief.has_value());
  EXPECT_EQ(brief.error().code(), ErrorCode::kSnapshotAlreadyInProgress);

  auto second_load = coordinator_.BeginLoad(nullptr);
  ASSERT_FALSE(second_load.has_value());
  EXPECT_EQ(second_load.error().code(), ErrorCode::kSnapshotAlreadyInProgress);
  EXPECT_TRUE(loading_.load());
}

TEST_F(SnapshotCoordinatorTest, ALockSaveRefusesEveryOtherSessionWithoutTouchingItsFlag) {
  {
    auto save = coordinator_.BeginLockSave(nullptr);
    ASSERT_TRUE(save.has_value()) << save.error().message();
    EXPECT_TRUE(read_only_.load());

    auto load = coordinator_.BeginLoad(nullptr);
    ASSERT_FALSE(load.has_value());
    EXPECT_EQ(load.error().code(), ErrorCode::kSnapshotAlreadyInProgress);
    EXPECT_FALSE(loading_.load());
    EXPECT_FALSE(coordinator_.BeginBriefSession().has_value());
    EXPECT_FALSE(coordinator_.BeginLockSave(nullptr).has_value());
  }
  EXPECT_FALSE(read_only_.load());
  EXPECT_TRUE(coordinator_.BeginLoad(nullptr).has_value());
  EXPECT_FALSE(loading_.load());
}

TEST_F(SnapshotCoordinatorTest, ConcurrentSaveAndLoadNeverBothProceed) {
  for (int round = 0; round < 200; ++round) {
    std::atomic<int> ready{0};
    std::atomic<int> attempted{0};
    std::atomic<bool> save_admitted{false};
    std::atomic<bool> load_admitted{false};
    const auto contend = [&](bool save) {
      ready.fetch_add(1);
      while (ready.load() < 2) {
        std::this_thread::yield();
      }
      if (save) {
        auto session = coordinator_.BeginLockSave(nullptr);
        save_admitted = session.has_value();
        attempted.fetch_add(1);
        while (attempted.load() < 2) {
          std::this_thread::yield();
        }
      } else {
        auto session = coordinator_.BeginLoad(nullptr);
        load_admitted = session.has_value();
        attempted.fetch_add(1);
        while (attempted.load() < 2) {
          std::this_thread::yield();
        }
      }
    };
    std::thread saver(contend, true);
    std::thread loader(contend, false);
    saver.join();
    loader.join();
    ASSERT_NE(save_admitted.load(), load_admitted.load()) << "round " << round;
    ASSERT_FALSE(read_only_.load());
    ASSERT_FALSE(loading_.load());
  }
}

TEST_F(SnapshotCoordinatorTest, LockSaveDrainsWritesButKeepsServingSearches) {
  auto admitted_writer = coordinator_.AcquireStoreAccess(StoreAccess::kWrite);
  auto save = std::async(std::launch::async, [this] { return coordinator_.BeginLockSave(nullptr).has_value(); });
  while (!read_only_.load()) {
    std::this_thread::yield();
  }
  // The save is waiting for the admitted writer; a search must not queue behind it.
  auto search = std::async(std::launch::async,
                           [this] { return coordinator_.AcquireStoreAccess(StoreAccess::kSearch).owns_lock(); });
  ASSERT_EQ(search.wait_for(kPrompt), std::future_status::ready);
  EXPECT_TRUE(search.get());
  EXPECT_EQ(save.wait_for(kBlocked), std::future_status::timeout);

  admitted_writer.unlock();
  ASSERT_EQ(save.wait_for(kPrompt), std::future_status::ready);
  EXPECT_TRUE(save.get());
}

TEST_F(SnapshotCoordinatorTest, AHeldLockSaveBlocksWritesOnly) {
  std::future<bool> write;
  {
    auto save = coordinator_.BeginLockSave(nullptr);
    ASSERT_TRUE(save.has_value());

    auto search = std::async(std::launch::async,
                             [this] { return coordinator_.AcquireStoreAccess(StoreAccess::kSearch).owns_lock(); });
    ASSERT_EQ(search.wait_for(kPrompt), std::future_status::ready);
    EXPECT_TRUE(search.get());

    write = std::async(std::launch::async,
                       [this] { return coordinator_.AcquireStoreAccess(StoreAccess::kWrite).owns_lock(); });
    EXPECT_EQ(write.wait_for(kBlocked), std::future_status::timeout);
  }
  ASSERT_EQ(write.wait_for(kPrompt), std::future_status::ready);
  EXPECT_TRUE(write.get());
}

TEST_F(SnapshotCoordinatorTest, LoadPublicationDrainsSearchesAndWrites) {
  auto load = coordinator_.BeginLoad(nullptr);
  ASSERT_TRUE(load.has_value());

  auto active_search = coordinator_.AcquireStoreAccess(StoreAccess::kSearch);
  auto publish = std::async(std::launch::async, [&load] { return load->Publish().searches.owns_lock(); });
  EXPECT_EQ(publish.wait_for(kBlocked), std::future_status::timeout);
  active_search.unlock();
  ASSERT_EQ(publish.wait_for(kPrompt), std::future_status::ready);
  EXPECT_TRUE(publish.get());
}

TEST_F(SnapshotCoordinatorTest, FailStopKeepsLoadingSetAfterTheLoadEnds) {
  {
    auto load = coordinator_.BeginLoad(nullptr);
    ASSERT_TRUE(load.has_value());
    load->KeepLoading();
  }
  EXPECT_TRUE(loading_.load());
  EXPECT_FALSE(coordinator_.BeginLockSave(nullptr).has_value());
}

TEST_F(SnapshotCoordinatorTest, ShutdownRefusesEverySession) {
  coordinator_.BeginShutdown();
  for (auto code : {coordinator_.BeginLockSave(nullptr).error().code(), coordinator_.BeginLoad(nullptr).error().code(),
                    coordinator_.BeginBriefSession().error().code()}) {
    EXPECT_EQ(code, ErrorCode::kCancelled);
  }
  EXPECT_FALSE(read_only_.load());
  EXPECT_FALSE(loading_.load());
}

TEST_F(SnapshotCoordinatorTest, ShutdownWaitsForABriefSessionInFlight) {
  std::future<void> shutdown;
  {
    auto brief = coordinator_.BeginBriefSession();
    ASSERT_TRUE(brief.has_value());
    shutdown = std::async(std::launch::async, [this] { coordinator_.BeginShutdown(); });
    EXPECT_EQ(shutdown.wait_for(kBlocked), std::future_status::timeout);
  }
  EXPECT_EQ(shutdown.wait_for(kPrompt), std::future_status::ready);
  EXPECT_EQ(coordinator_.BeginBriefSession().error().code(), ErrorCode::kCancelled);
}

/// A server context with real stores, a WAL and a fork writer.
class SnapshotCoordinatorServerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = std::filesystem::temp_directory_path() / ("nvecd_coordinator_test_" + std::to_string(::getpid()));
    std::filesystem::remove_all(dir_);
    std::filesystem::create_directories(dir_ / "wal");
    std::filesystem::permissions(dir_, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace);

    config_.snapshot.dir = dir_.string();
    storage::WriteAheadLog::Config wal_config;
    wal_config.directory = (dir_ / "wal").string();
    wal_config.sync_on_write = true;
    ASSERT_TRUE(wal_.Open(wal_config));
    ctx_.wal = &wal_;
    ctx_.dump_dir = dir_.string();
    ctx_.fork_snapshot_writer = &fork_writer_;
    fork_writer_.SetWal(&wal_);
  }

  void TearDown() override {
    fork_writer_.WaitForChild();
    wal_.Close();
    std::filesystem::remove_all(dir_);
  }

  std::filesystem::path dir_;
  std::atomic<bool> loading_{false};
  std::atomic<bool> read_only_{false};
  ServerStats stats_;
  config::Config config_;
  events::EventStore event_store_{config_.events};
  events::CoOccurrenceIndex co_index_;
  vectors::VectorStore vector_store_{config_.vectors};
  storage::WriteAheadLog wal_;
  storage::ForkSnapshotWriter fork_writer_;
  HandlerContext ctx_{&event_store_, &co_index_, &vector_store_, nullptr,    nullptr, nullptr, nullptr,
                      stats_,        &config_,   loading_,       read_only_, "",      ""};
};

TEST_F(SnapshotCoordinatorServerTest, LockModeSaveIsRefusedWhileAForkChildIsWriting) {
  // Enough data that the child is still writing when it is stopped.
  for (int index = 0; index < 50000; ++index) {
    ASSERT_TRUE(vector_store_.SetVector("item" + std::to_string(index), std::vector<float>(32, 1.0F)));
  }
  ASSERT_TRUE(wal_.Append(storage::WalOpType::kVecSet, nullptr, 0));
  const uint64_t sequence_before = wal_.CurrentSequence();

  config_.snapshot.mode = "fork";
  ASSERT_TRUE(handlers::StartForkSnapshot(ctx_, (dir_ / "auto_fork.nvec").string()));
  const pid_t child = fork_writer_.GetStatus().child_pid;
  ASSERT_GT(child, 0);
  ASSERT_EQ(::kill(child, SIGSTOP), 0);
  fork_writer_.CheckChild();
  ASSERT_TRUE(fork_writer_.IsInProgress()) << "child finished before it could be stopped";

  config_.snapshot.mode = "lock";
  auto refused = handlers::HandleDumpSave(ctx_, "lock.nvec");
  ASSERT_EQ(::kill(child, SIGCONT), 0);

  ASSERT_FALSE(refused.has_value());
  EXPECT_EQ(refused.error().code(), ErrorCode::kSnapshotAlreadyInProgress);
  EXPECT_FALSE(read_only_.load());
  EXPECT_FALSE(std::filesystem::exists(dir_ / "lock.nvec"));
  EXPECT_FALSE(std::filesystem::exists(storage::WalCheckpointPath((dir_ / "lock.nvec").string())));
  EXPECT_EQ(wal_.CurrentSequence(), sequence_before);
}

TEST_F(SnapshotCoordinatorServerTest, EveryDumpSaveFormIsRefusedOnceShutdownBegins) {
  ctx_.snapshot_coordinator.BeginShutdown();
  for (const char* mode : {"lock", "fork"}) {
    config_.snapshot.mode = mode;
    auto refused = handlers::HandleDumpSave(ctx_, "late.nvec");
    ASSERT_FALSE(refused.has_value()) << mode;
    EXPECT_EQ(refused.error().code(), ErrorCode::kCancelled) << mode;
  }
  EXPECT_FALSE(fork_writer_.IsInProgress());
  EXPECT_FALSE(std::filesystem::exists(dir_ / "late.nvec"));
}

}  // namespace
}  // namespace nvecd::server
