/**
 * @file snapshot_file_checksum_test.cpp
 * @brief The whole-file CRC32 is accumulated while writing and reading, and must agree with a pass over the file
 */

#include <gtest/gtest.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <unordered_map>

#include "config/config.h"
#include "events/co_occurrence_index.h"
#include "events/event_store.h"
#include "storage/snapshot_format.h"
#include "storage/snapshot_format_v1.h"
#include "vectors/metadata_store.h"
#include "vectors/vector_store.h"

namespace nvecd::storage::snapshot_v1 {
namespace {

namespace fs = std::filesystem;

/// Byte offset of the snapshot timestamp, which no section checksum covers.
constexpr std::streamoff kTimestampOffset = snapshot_format::kFixedHeaderSize + 8;

class SnapshotFileChecksumTest : public ::testing::Test {
 protected:
  void SetUp() override {
    test_dir_ = fs::temp_directory_path() / ("nvecd_snapshot_file_checksum_test_" + std::to_string(::getpid()));
    fs::remove_all(test_dir_);
    fs::create_directories(test_dir_);

    event_store_ = std::make_unique<events::EventStore>(config::EventsConfig{});
    co_index_ = std::make_unique<events::CoOccurrenceIndex>();
    vector_store_ = std::make_unique<vectors::VectorStore>(config::VectorsConfig{});
    metadata_store_ = std::make_unique<vectors::MetadataStore>();

    ASSERT_TRUE(event_store_->AddEvent("ctx", "item1", 10, events::EventType::ADD).has_value());
    ASSERT_TRUE(event_store_->AddEvent("ctx", "item2", 20, events::EventType::ADD).has_value());
    ASSERT_TRUE(vector_store_->SetVector("item1", {0.1F, 0.2F, 0.3F}).has_value());
    ASSERT_TRUE(vector_store_->SetVector("item2", {0.4F, 0.5F, 0.6F}).has_value());
    metadata_store_->Set("item1", {{"kind", std::string("a")}});
  }

  void TearDown() override { fs::remove_all(test_dir_); }

  std::string Path(const std::string& name) const { return (test_dir_ / name).string(); }

  Expected<void, Error> Write(const std::string& path, bool with_statistics) {
    SnapshotStatistics statistics;
    std::unordered_map<std::string, StoreStatistics> store_statistics;
    store_statistics["events"] = StoreStatistics{};
    store_statistics["co_occurrence"] = StoreStatistics{};
    store_statistics["vectors"] = StoreStatistics{};
    return WriteSnapshotV1(path, config_, *event_store_, *co_index_, *vector_store_,
                           with_statistics ? &statistics : nullptr, with_statistics ? &store_statistics : nullptr,
                           metadata_store_.get());
  }

  Expected<void, Error> Read(const std::string& path, snapshot_format::IntegrityError* integrity_error = nullptr) {
    events::EventStore events(config::EventsConfig{});
    events::CoOccurrenceIndex co;
    vectors::VectorStore vectors(config::VectorsConfig{});
    vectors::MetadataStore metadata;
    config::Config loaded;
    SnapshotStatistics statistics;
    std::unordered_map<std::string, StoreStatistics> store_statistics;
    return ReadSnapshotV1(path, loaded, events, co, vectors, &statistics, &store_statistics, integrity_error,
                          &metadata);
  }

  fs::path test_dir_;
  config::Config config_;
  std::unique_ptr<events::EventStore> event_store_;
  std::unique_ptr<events::CoOccurrenceIndex> co_index_;
  std::unique_ptr<vectors::VectorStore> vector_store_;
  std::unique_ptr<vectors::MetadataStore> metadata_store_;
};

TEST_F(SnapshotFileChecksumTest, WrittenChecksumMatchesAPassOverTheFile) {
  for (const bool with_statistics : {false, true}) {
    const std::string path = Path(with_statistics ? "with_statistics.dmp" : "plain.dmp");
    ASSERT_TRUE(Write(path, with_statistics).has_value());

    snapshot_format::IntegrityError integrity_error;
    auto verified = VerifySnapshotIntegrity(path, integrity_error);
    EXPECT_TRUE(verified.has_value()) << integrity_error.message;
  }
}

TEST_F(SnapshotFileChecksumTest, ReadAcceptsEveryOptionalSection) {
  for (const bool with_statistics : {false, true}) {
    const std::string path = Path(with_statistics ? "read_with_statistics.dmp" : "read_plain.dmp");
    ASSERT_TRUE(Write(path, with_statistics).has_value());

    snapshot_format::IntegrityError integrity_error;
    auto read = Read(path, &integrity_error);
    EXPECT_TRUE(read.has_value()) << (read ? "" : read.error().message()) << " " << integrity_error.message;
  }
}

TEST_F(SnapshotFileChecksumTest, DamageOutsideEverySectionIsStillCaught) {
  const std::string path = Path("header_damage.dmp");
  ASSERT_TRUE(Write(path, /*with_statistics=*/true).has_value());

  // The timestamp sits in the header, so every section checksum stays valid and
  // only the whole-file checksum can notice the change.
  {
    std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
    ASSERT_TRUE(file.is_open());
    file.seekp(kTimestampOffset);
    const char flipped = static_cast<char>(0x5A);
    file.write(&flipped, 1);
  }

  snapshot_format::IntegrityError integrity_error;
  auto read = Read(path, &integrity_error);
  ASSERT_FALSE(read.has_value());
  EXPECT_EQ(integrity_error.type, snapshot_format::CRCErrorType::FileCRC);
}

TEST_F(SnapshotFileChecksumTest, SectionDamageIsReportedAsFileDamage) {
  const std::string path = Path("section_damage.dmp");
  ASSERT_TRUE(Write(path, /*with_statistics=*/false).has_value());

  {
    std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
    ASSERT_TRUE(file.is_open());
    file.seekg(0, std::ios::end);
    const auto last = static_cast<std::streamoff>(file.tellg()) - 1;
    file.seekg(last);
    char byte = 0;
    file.read(&byte, 1);
    byte = static_cast<char>(byte ^ 0xFF);
    file.seekp(last);
    file.write(&byte, 1);
  }

  snapshot_format::IntegrityError integrity_error;
  auto read = Read(path, &integrity_error);
  ASSERT_FALSE(read.has_value());
  EXPECT_EQ(integrity_error.type, snapshot_format::CRCErrorType::FileCRC);
}

}  // namespace
}  // namespace nvecd::storage::snapshot_v1
