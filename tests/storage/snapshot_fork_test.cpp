/**
 * @file snapshot_fork_test.cpp
 * @brief Unit tests for fork-based COW snapshot
 */

#include "storage/snapshot_fork.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <thread>

#include "config/config.h"
#include "events/co_occurrence_index.h"
#include "events/event_store.h"
#include "storage/snapshot_format_v1.h"
#include "vectors/vector_store.h"

using namespace nvecd;

class SnapshotForkTest : public ::testing::Test {
 protected:
  void SetUp() override {
    config_.events.ctx_buffer_size = 50;
    config_.vectors.default_dimension = 3;
    config_.vectors.distance_metric = "cosine";

    event_store_ = std::make_unique<events::EventStore>(config_.events);
    co_index_ = std::make_unique<events::CoOccurrenceIndex>();
    vector_store_ = std::make_unique<vectors::VectorStore>(config_.vectors);

    // Add some test data
    event_store_->AddEvent("ctx1", "item1", 10);
    event_store_->AddEvent("ctx1", "item2", 20);
    vector_store_->SetVector("item1", {1.0f, 0.0f, 0.0f});
    vector_store_->SetVector("item2", {0.0f, 1.0f, 0.0f});

    snapshot_path_ = "/tmp/nvecd_fork_test_snapshot.dmp";
  }

  void TearDown() override { std::filesystem::remove(snapshot_path_); }

  config::Config config_;
  std::unique_ptr<events::EventStore> event_store_;
  std::unique_ptr<events::CoOccurrenceIndex> co_index_;
  std::unique_ptr<vectors::VectorStore> vector_store_;
  std::string snapshot_path_;
};

TEST_F(SnapshotForkTest, InitialStatusIsIdle) {
  storage::ForkSnapshotWriter writer;
  auto status = writer.GetStatus();
  EXPECT_EQ(status.status, storage::SnapshotStatus::kIdle);
  EXPECT_FALSE(writer.IsInProgress());
}

TEST_F(SnapshotForkTest, BackgroundSaveCompletes) {
  storage::ForkSnapshotWriter writer;

  auto result = writer.StartBackgroundSave(snapshot_path_, config_, *event_store_, *co_index_, *vector_store_);
  ASSERT_TRUE(result) << result.error().message();

  // Wait for child to finish
  writer.WaitForChild(10000);

  auto status = writer.GetStatus();
  EXPECT_EQ(status.status, storage::SnapshotStatus::kCompleted) << "Error: " << status.error_message;
  EXPECT_TRUE(std::filesystem::exists(snapshot_path_));
}

TEST_F(SnapshotForkTest, SnapshotCanBeReadBack) {
  storage::ForkSnapshotWriter writer;

  auto result = writer.StartBackgroundSave(snapshot_path_, config_, *event_store_, *co_index_, *vector_store_);
  ASSERT_TRUE(result) << result.error().message();

  writer.WaitForChild(10000);

  auto status = writer.GetStatus();
  ASSERT_EQ(status.status, storage::SnapshotStatus::kCompleted) << "Error: " << status.error_message;

  // Read back
  config::Config loaded_config;
  events::EventStore loaded_es(config_.events);
  events::CoOccurrenceIndex loaded_co;
  vectors::VectorStore loaded_vs(config_.vectors);

  auto read_result =
      storage::snapshot_v1::ReadSnapshotV1(snapshot_path_, loaded_config, loaded_es, loaded_co, loaded_vs);
  ASSERT_TRUE(read_result) << read_result.error().message();

  EXPECT_EQ(loaded_vs.GetVectorCount(), 2);
  EXPECT_TRUE(loaded_vs.GetVector("item1").has_value());
  EXPECT_TRUE(loaded_vs.GetVector("item2").has_value());
}

TEST_F(SnapshotForkTest, RejectsSecondConcurrentSave) {
  storage::ForkSnapshotWriter writer;

  auto result1 = writer.StartBackgroundSave(snapshot_path_, config_, *event_store_, *co_index_, *vector_store_);
  ASSERT_TRUE(result1) << result1.error().message();

  // Try to start another while first is in progress
  auto result2 = writer.StartBackgroundSave("/tmp/nvecd_fork_test_snapshot2.dmp", config_, *event_store_, *co_index_,
                                            *vector_store_);
  EXPECT_FALSE(result2);
  EXPECT_EQ(result2.error().code(), utils::ErrorCode::kSnapshotAlreadyInProgress);

  writer.WaitForChild(10000);
  std::filesystem::remove("/tmp/nvecd_fork_test_snapshot2.dmp");
}

TEST_F(SnapshotForkTest, CheckChildUpdatesStatus) {
  storage::ForkSnapshotWriter writer;

  auto result = writer.StartBackgroundSave(snapshot_path_, config_, *event_store_, *co_index_, *vector_store_);
  ASSERT_TRUE(result) << result.error().message();

  EXPECT_TRUE(writer.IsInProgress());

  // Wait and then check
  writer.WaitForChild(10000);
  writer.CheckChild();

  EXPECT_FALSE(writer.IsInProgress());
  auto status = writer.GetStatus();
  EXPECT_NE(status.status, storage::SnapshotStatus::kInProgress);
}

TEST_F(SnapshotForkTest, EmptyStoresWork) {
  events::EventStore empty_es(config_.events);
  events::CoOccurrenceIndex empty_co;
  vectors::VectorStore empty_vs(config_.vectors);

  storage::ForkSnapshotWriter writer;
  auto result = writer.StartBackgroundSave(snapshot_path_, config_, empty_es, empty_co, empty_vs);
  ASSERT_TRUE(result) << result.error().message();

  writer.WaitForChild(10000);
  auto status = writer.GetStatus();
  EXPECT_EQ(status.status, storage::SnapshotStatus::kCompleted) << "Error: " << status.error_message;
}
