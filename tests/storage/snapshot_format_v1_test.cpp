/**
 * @file snapshot_format_v1_test.cpp
 * @brief Unit tests for snapshot format V1 serialization/deserialization
 *
 * Tests the core snapshot format functions in isolation without requiring
 * a running server. Covers round-trip, integrity verification, metadata
 * retrieval, error cases, and edge cases.
 */

#include "storage/snapshot_format_v1.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "config/config.h"
#include "events/co_occurrence_index.h"
#include "events/event_store.h"
#include "storage/snapshot_format.h"
#include "vectors/vector_store.h"

namespace fs = std::filesystem;

using namespace nvecd;
using namespace nvecd::storage;
using namespace nvecd::storage::snapshot_v1;

class SnapshotFormatV1Test : public ::testing::Test {
 protected:
  void SetUp() override {
    // Create a unique temp directory for each test
    test_dir_ = fs::temp_directory_path() /
                ("nvecd_snapshot_v1_test_" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()));
    fs::create_directories(test_dir_);

    // Create stores with default configs
    config::EventsConfig events_cfg;
    config::VectorsConfig vectors_cfg;
    event_store_ = std::make_unique<events::EventStore>(events_cfg);
    co_index_ = std::make_unique<events::CoOccurrenceIndex>();
    vector_store_ = std::make_unique<vectors::VectorStore>(vectors_cfg);
  }

  void TearDown() override {
    if (fs::exists(test_dir_)) {
      fs::remove_all(test_dir_);
    }
  }

  std::string TestFilePath(const std::string& name) { return (test_dir_ / name).string(); }

  /// @brief Populate stores with test data for round-trip verification
  void PopulateStores() {
    // Add events to two contexts
    auto r1 = event_store_->AddEvent("ctx1", "item1", 100, events::EventType::ADD);
    ASSERT_TRUE(r1.has_value()) << "AddEvent ctx1/item1 failed";
    auto r2 = event_store_->AddEvent("ctx1", "item2", 80, events::EventType::ADD);
    ASSERT_TRUE(r2.has_value()) << "AddEvent ctx1/item2 failed";
    auto r3 = event_store_->AddEvent("ctx2", "item2", 90, events::EventType::ADD);
    ASSERT_TRUE(r3.has_value()) << "AddEvent ctx2/item2 failed";
    auto r4 = event_store_->AddEvent("ctx2", "item3", 70, events::EventType::ADD);
    ASSERT_TRUE(r4.has_value()) << "AddEvent ctx2/item3 failed";

    // Build co-occurrence from events
    auto events_ctx1 = event_store_->GetEvents("ctx1");
    co_index_->UpdateFromEvents("ctx1", events_ctx1);
    auto events_ctx2 = event_store_->GetEvents("ctx2");
    co_index_->UpdateFromEvents("ctx2", events_ctx2);

    // Add vectors (3-dimensional for simplicity)
    auto v1 = vector_store_->SetVector("item1", {0.1f, 0.2f, 0.3f});
    ASSERT_TRUE(v1.has_value()) << "SetVector item1 failed";
    auto v2 = vector_store_->SetVector("item2", {0.4f, 0.5f, 0.6f});
    ASSERT_TRUE(v2.has_value()) << "SetVector item2 failed";
    auto v3 = vector_store_->SetVector("item3", {0.7f, 0.8f, 0.9f});
    ASSERT_TRUE(v3.has_value()) << "SetVector item3 failed";
  }

  fs::path test_dir_;
  config::Config config_;
  std::unique_ptr<events::EventStore> event_store_;
  std::unique_ptr<events::CoOccurrenceIndex> co_index_;
  std::unique_ptr<vectors::VectorStore> vector_store_;
};

// ---------------------------------------------------------------------------
// 1. WriteAndRead_RoundTrip
// ---------------------------------------------------------------------------
TEST_F(SnapshotFormatV1Test, WriteAndRead_RoundTrip) {
  PopulateStores();

  const std::string path = TestFilePath("round_trip.dmp");

  // Write snapshot
  auto write_result = WriteSnapshotV1(path, config_, *event_store_, *co_index_, *vector_store_);
  ASSERT_TRUE(write_result.has_value()) << "WriteSnapshotV1 failed: " << write_result.error().message();

  // Create fresh stores for reading
  config::EventsConfig events_cfg;
  config::VectorsConfig vectors_cfg;
  events::EventStore loaded_events(events_cfg);
  events::CoOccurrenceIndex loaded_co;
  vectors::VectorStore loaded_vectors(vectors_cfg);
  config::Config loaded_config;

  auto read_result = ReadSnapshotV1(path, loaded_config, loaded_events, loaded_co, loaded_vectors);
  ASSERT_TRUE(read_result.has_value()) << "ReadSnapshotV1 failed: " << read_result.error().message();

  // Verify event store round-trip
  EXPECT_EQ(loaded_events.GetContextCount(), event_store_->GetContextCount());

  auto orig_ctx1 = event_store_->GetEvents("ctx1");
  auto loaded_ctx1 = loaded_events.GetEvents("ctx1");
  ASSERT_EQ(loaded_ctx1.size(), orig_ctx1.size());
  for (size_t i = 0; i < orig_ctx1.size(); ++i) {
    EXPECT_EQ(loaded_ctx1[i].item_id, orig_ctx1[i].item_id);
    EXPECT_EQ(loaded_ctx1[i].score, orig_ctx1[i].score);
  }

  auto orig_ctx2 = event_store_->GetEvents("ctx2");
  auto loaded_ctx2 = loaded_events.GetEvents("ctx2");
  ASSERT_EQ(loaded_ctx2.size(), orig_ctx2.size());

  // Verify co-occurrence round-trip
  EXPECT_EQ(loaded_co.GetItemCount(), co_index_->GetItemCount());
  // Check a known co-occurrence score
  float orig_score = co_index_->GetScore("item1", "item2");
  float loaded_score = loaded_co.GetScore("item1", "item2");
  EXPECT_FLOAT_EQ(loaded_score, orig_score);

  float orig_score2 = co_index_->GetScore("item2", "item3");
  float loaded_score2 = loaded_co.GetScore("item2", "item3");
  EXPECT_FLOAT_EQ(loaded_score2, orig_score2);

  // Verify vector store round-trip
  EXPECT_EQ(loaded_vectors.GetVectorCount(), vector_store_->GetVectorCount());

  auto orig_vec1 = vector_store_->GetVector("item1");
  auto loaded_vec1 = loaded_vectors.GetVector("item1");
  ASSERT_TRUE(orig_vec1.has_value());
  ASSERT_TRUE(loaded_vec1.has_value());
  ASSERT_EQ(loaded_vec1->data.size(), orig_vec1->data.size());
  for (size_t i = 0; i < orig_vec1->data.size(); ++i) {
    EXPECT_FLOAT_EQ(loaded_vec1->data[i], orig_vec1->data[i]);
  }
}

// ---------------------------------------------------------------------------
// 2. WriteAndVerify_IntegrityOk
// ---------------------------------------------------------------------------
TEST_F(SnapshotFormatV1Test, WriteAndVerify_IntegrityOk) {
  PopulateStores();

  const std::string path = TestFilePath("integrity_ok.dmp");

  auto write_result = WriteSnapshotV1(path, config_, *event_store_, *co_index_, *vector_store_);
  ASSERT_TRUE(write_result.has_value()) << "WriteSnapshotV1 failed: " << write_result.error().message();

  snapshot_format::IntegrityError integrity_error;
  auto verify_result = VerifySnapshotIntegrity(path, integrity_error);
  EXPECT_TRUE(verify_result.has_value()) << "VerifySnapshotIntegrity failed: " << verify_result.error().message();
  EXPECT_FALSE(integrity_error.HasError());
  EXPECT_EQ(integrity_error.type, snapshot_format::CRCErrorType::None);
}

// ---------------------------------------------------------------------------
// 3. GetSnapshotInfo_ValidFile
// ---------------------------------------------------------------------------
TEST_F(SnapshotFormatV1Test, GetSnapshotInfo_ValidFile) {
  PopulateStores();

  const std::string path = TestFilePath("info_valid.dmp");

  auto write_result = WriteSnapshotV1(path, config_, *event_store_, *co_index_, *vector_store_);
  ASSERT_TRUE(write_result.has_value()) << "WriteSnapshotV1 failed: " << write_result.error().message();

  SnapshotInfo info;
  auto info_result = GetSnapshotInfo(path, info);
  ASSERT_TRUE(info_result.has_value()) << "GetSnapshotInfo failed: " << info_result.error().message();

  EXPECT_EQ(info.version, snapshot_format::kCurrentVersion);
  EXPECT_EQ(info.store_count, 3u);  // events, co_occurrence, vectors
  EXPECT_GT(info.file_size, 0u);
  EXPECT_GT(info.timestamp, 0u);
  // CRC flag should always be set in V1
  EXPECT_TRUE(info.flags & snapshot_format::flags_v1::kWithCRC);
}

// ---------------------------------------------------------------------------
// 4. ReadNonexistent_ReturnsError
// ---------------------------------------------------------------------------
TEST_F(SnapshotFormatV1Test, ReadNonexistent_ReturnsError) {
  const std::string path = TestFilePath("does_not_exist.dmp");

  config::EventsConfig events_cfg;
  config::VectorsConfig vectors_cfg;
  events::EventStore loaded_events(events_cfg);
  events::CoOccurrenceIndex loaded_co;
  vectors::VectorStore loaded_vectors(vectors_cfg);
  config::Config loaded_config;

  auto read_result = ReadSnapshotV1(path, loaded_config, loaded_events, loaded_co, loaded_vectors);
  EXPECT_FALSE(read_result.has_value());
}

// ---------------------------------------------------------------------------
// 5. VerifyNonexistent_ReturnsError
// ---------------------------------------------------------------------------
TEST_F(SnapshotFormatV1Test, VerifyNonexistent_ReturnsError) {
  const std::string path = TestFilePath("does_not_exist.dmp");

  snapshot_format::IntegrityError integrity_error;
  auto verify_result = VerifySnapshotIntegrity(path, integrity_error);
  EXPECT_FALSE(verify_result.has_value());
}

// ---------------------------------------------------------------------------
// 6. VerifyCorrupted_ReturnsError
// ---------------------------------------------------------------------------
TEST_F(SnapshotFormatV1Test, VerifyCorrupted_ReturnsError) {
  PopulateStores();

  const std::string path = TestFilePath("corrupted.dmp");

  auto write_result = WriteSnapshotV1(path, config_, *event_store_, *co_index_, *vector_store_);
  ASSERT_TRUE(write_result.has_value()) << "WriteSnapshotV1 failed: " << write_result.error().message();

  // Read the file, corrupt a byte in the middle, write it back
  std::string file_data;
  {
    std::ifstream ifs(path, std::ios::binary);
    ASSERT_TRUE(ifs.is_open());
    file_data.assign(std::istreambuf_iterator<char>(ifs), std::istreambuf_iterator<char>());
  }
  ASSERT_GT(file_data.size(), 100u);

  // Flip a byte roughly in the middle of the data section
  size_t corrupt_offset = file_data.size() / 2;
  file_data[corrupt_offset] ^= 0xFF;

  {
    std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(ofs.is_open());
    ofs.write(file_data.data(), static_cast<std::streamsize>(file_data.size()));
  }

  snapshot_format::IntegrityError integrity_error;
  auto verify_result = VerifySnapshotIntegrity(path, integrity_error);

  // Either the function returns an error or the integrity_error is set
  EXPECT_TRUE(!verify_result.has_value() || integrity_error.HasError()) << "Corrupted file should fail verification";
}

// ---------------------------------------------------------------------------
// 7. GetSnapshotInfo_NonexistentFile
// ---------------------------------------------------------------------------
TEST_F(SnapshotFormatV1Test, GetSnapshotInfo_NonexistentFile) {
  const std::string path = TestFilePath("nonexistent_info.dmp");

  SnapshotInfo info;
  auto info_result = GetSnapshotInfo(path, info);
  EXPECT_FALSE(info_result.has_value());
}

// ---------------------------------------------------------------------------
// 8. WriteEmptyStores
// ---------------------------------------------------------------------------
TEST_F(SnapshotFormatV1Test, WriteEmptyStores) {
  // Do NOT populate stores - test with empty data
  const std::string path = TestFilePath("empty_stores.dmp");

  auto write_result = WriteSnapshotV1(path, config_, *event_store_, *co_index_, *vector_store_);
  ASSERT_TRUE(write_result.has_value()) << "WriteSnapshotV1 failed: " << write_result.error().message();

  // Verify the file was created
  EXPECT_TRUE(fs::exists(path));

  // Read it back into fresh stores
  config::EventsConfig events_cfg;
  config::VectorsConfig vectors_cfg;
  events::EventStore loaded_events(events_cfg);
  events::CoOccurrenceIndex loaded_co;
  vectors::VectorStore loaded_vectors(vectors_cfg);
  config::Config loaded_config;

  auto read_result = ReadSnapshotV1(path, loaded_config, loaded_events, loaded_co, loaded_vectors);
  ASSERT_TRUE(read_result.has_value()) << "ReadSnapshotV1 failed: " << read_result.error().message();

  // All stores should be empty
  EXPECT_EQ(loaded_events.GetContextCount(), 0u);
  EXPECT_EQ(loaded_co.GetItemCount(), 0u);
  EXPECT_EQ(loaded_vectors.GetVectorCount(), 0u);

  // Integrity should pass
  snapshot_format::IntegrityError integrity_error;
  auto verify_result = VerifySnapshotIntegrity(path, integrity_error);
  EXPECT_TRUE(verify_result.has_value()) << "VerifySnapshotIntegrity failed: " << verify_result.error().message();
  EXPECT_FALSE(integrity_error.HasError());
}
