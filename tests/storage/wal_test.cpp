/**
 * @file wal_test.cpp
 * @brief Tests for Write-Ahead Log
 */

#include "storage/wal.h"

#include <fcntl.h>
#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace nvecd::storage {
namespace {

namespace fs = std::filesystem;

bool RewriteRecordSequence(const std::string& path, size_t record_index, uint64_t sequence) {
  const int fd = ::open(path.c_str(), O_RDWR);
  if (fd < 0) {
    return false;
  }

  off_t offset = kWalFileHeaderSize;
  for (size_t index = 0; index <= record_index; ++index) {
    uint8_t header[8]{};
    if (::pread(fd, header, sizeof(header), offset) != static_cast<ssize_t>(sizeof(header))) {
      ::close(fd);
      return false;
    }
    uint32_t body_length = 0;
    std::memcpy(&body_length, header, sizeof(body_length));
    if (body_length < kWalRecordHeaderSize || body_length > kMaxWalRecordBodySize) {
      ::close(fd);
      return false;
    }
    if (index != record_index) {
      offset += static_cast<off_t>(sizeof(header) + body_length);
      continue;
    }

    std::vector<uint8_t> body(body_length);
    if (::pread(fd, body.data(), body.size(), offset + static_cast<off_t>(sizeof(header))) !=
        static_cast<ssize_t>(body.size())) {
      ::close(fd);
      return false;
    }
    std::memcpy(body.data(), &sequence, sizeof(sequence));
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    const auto crc = static_cast<uint32_t>(crc32(0L, reinterpret_cast<const Bytef*>(body.data()), body.size()));
    std::memcpy(header + sizeof(uint32_t), &crc, sizeof(crc));

    const bool body_written = ::pwrite(fd, body.data(), body.size(), offset + static_cast<off_t>(sizeof(header))) ==
                              static_cast<ssize_t>(body.size());
    const bool header_written = ::pwrite(fd, header, sizeof(header), offset) == static_cast<ssize_t>(sizeof(header));
    const bool closed = ::close(fd) == 0;
    return body_written && header_written && closed;
  }

  ::close(fd);
  return false;
}

class WalTest : public ::testing::Test {
 protected:
  void SetUp() override {
    test_dir_ = "/tmp/nvecd_wal_test_" + std::to_string(::getpid());
    fs::remove_all(test_dir_);
    fs::create_directories(test_dir_);
  }

  void TearDown() override { fs::remove_all(test_dir_); }

  WriteAheadLog::Config MakeConfig(uint64_t max_file_size = 64 * 1024 * 1024) {
    WriteAheadLog::Config cfg;
    cfg.directory = test_dir_;
    cfg.max_file_size = max_file_size;
    cfg.sync_on_write = true;  // Deterministic in tests
    cfg.include_vectors = true;
    return cfg;
  }

  std::string test_dir_;
};

// --- Basic Operations ---

TEST_F(WalTest, OpenClose) {
  WriteAheadLog wal;
  auto result = wal.Open(MakeConfig());
  ASSERT_TRUE(result.has_value()) << result.error().to_string();
  EXPECT_TRUE(wal.IsOpen());
  EXPECT_EQ(wal.CurrentSequence(), 0U);
  wal.Close();
  EXPECT_FALSE(wal.IsOpen());
}

TEST_F(WalTest, CreatesPrivateSegments) {
  WriteAheadLog wal;
  ASSERT_TRUE(wal.Open(MakeConfig()));

  struct stat info {};
  ASSERT_EQ(::stat((test_dir_ + "/wal-000001.log").c_str(), &info), 0);
  EXPECT_EQ(info.st_mode & (S_IRWXG | S_IRWXO), 0);
}

TEST_F(WalTest, ReopenClosesPreviousSegmentWithoutDeadlock) {
  WriteAheadLog wal;
  auto config = MakeConfig();
  ASSERT_TRUE(wal.Open(config));
  ASSERT_TRUE(wal.Append(WalOpType::kEventAdd, nullptr, 0));

  // Open must safely replace an existing lifecycle without recursively taking
  // the non-recursive WAL mutex.
  ASSERT_TRUE(wal.Open(config));
  EXPECT_TRUE(wal.IsOpen());
  EXPECT_TRUE(wal.Append(WalOpType::kEventAdd, nullptr, 0));
}

TEST_F(WalTest, ReopenRemovesHeaderOnlySegments) {
  const auto config = MakeConfig();
  for (int i = 0; i < 3; ++i) {
    WriteAheadLog wal;
    ASSERT_TRUE(wal.Open(config));
    wal.Close();
  }

  size_t segment_count = 0;
  for (const auto& entry : fs::directory_iterator(test_dir_)) {
    if (entry.path().filename().string().rfind("wal-", 0) == 0) {
      ++segment_count;
    }
  }
  EXPECT_EQ(segment_count, 1U);
}

TEST_F(WalTest, AppendSingle) {
  WriteAheadLog wal;
  ASSERT_TRUE(wal.Open(MakeConfig()));

  std::string payload = "test_id";
  auto result = wal.Append(WalOpType::kVecSet, payload.data(), static_cast<uint32_t>(payload.size()));
  ASSERT_TRUE(result.has_value()) << result.error().to_string();
  EXPECT_EQ(*result, 1U);
  EXPECT_EQ(wal.CurrentSequence(), 1U);
}

TEST_F(WalTest, AppendMultiple) {
  WriteAheadLog wal;
  ASSERT_TRUE(wal.Open(MakeConfig()));

  for (int i = 0; i < 100; ++i) {
    std::string payload = "item_" + std::to_string(i);
    auto result = wal.Append(WalOpType::kEventAdd, payload.data(), static_cast<uint32_t>(payload.size()));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, static_cast<uint64_t>(i + 1));
  }
  EXPECT_EQ(wal.CurrentSequence(), 100U);
}

TEST_F(WalTest, AppendWithoutOpen) {
  WriteAheadLog wal;
  std::string payload = "test";
  auto result = wal.Append(WalOpType::kVecSet, payload.data(), static_cast<uint32_t>(payload.size()));
  EXPECT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), utils::ErrorCode::kWalNotOpen);
}

// --- Replay ---

TEST_F(WalTest, AppendAndReplay) {
  // Write records
  {
    WriteAheadLog wal;
    ASSERT_TRUE(wal.Open(MakeConfig()));

    std::string p1 = "vec_001";
    ASSERT_TRUE(wal.Append(WalOpType::kVecSet, p1.data(), static_cast<uint32_t>(p1.size())));

    std::string p2 = "vec_002";
    ASSERT_TRUE(wal.Append(WalOpType::kVecDel, p2.data(), static_cast<uint32_t>(p2.size())));

    std::string p3 = "event_data";
    ASSERT_TRUE(wal.Append(WalOpType::kEventAdd, p3.data(), static_cast<uint32_t>(p3.size())));
  }

  // Replay all
  {
    WriteAheadLog wal;
    ASSERT_TRUE(wal.Open(MakeConfig()));

    std::vector<WalRecord> records;
    auto result = wal.Replay(1, [&](const WalRecord& r) { records.push_back(r); });
    ASSERT_TRUE(result.has_value()) << result.error().to_string();
    EXPECT_EQ(*result, 3U);
    ASSERT_EQ(records.size(), 3U);

    EXPECT_EQ(records[0].sequence, 1U);
    EXPECT_EQ(records[0].op, WalOpType::kVecSet);
    EXPECT_EQ(std::string(records[0].payload.begin(), records[0].payload.end()), "vec_001");

    EXPECT_EQ(records[1].sequence, 2U);
    EXPECT_EQ(records[1].op, WalOpType::kVecDel);

    EXPECT_EQ(records[2].sequence, 3U);
    EXPECT_EQ(records[2].op, WalOpType::kEventAdd);
  }
}

TEST_F(WalTest, ReplayFromMiddle) {
  {
    WriteAheadLog wal;
    ASSERT_TRUE(wal.Open(MakeConfig()));

    for (int i = 0; i < 10; ++i) {
      std::string p = "item_" + std::to_string(i);
      ASSERT_TRUE(wal.Append(WalOpType::kVecSet, p.data(), static_cast<uint32_t>(p.size())));
    }
  }

  {
    WriteAheadLog wal;
    ASSERT_TRUE(wal.Open(MakeConfig()));

    std::vector<WalRecord> records;
    auto result = wal.Replay(6, [&](const WalRecord& r) { records.push_back(r); });
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 5U);  // sequences 6,7,8,9,10
    EXPECT_EQ(records.front().sequence, 6U);
    EXPECT_EQ(records.back().sequence, 10U);
  }
}

TEST_F(WalTest, ReplayRejectsMissingRequestedFloor) {
  const auto config = MakeConfig(128);
  {
    WriteAheadLog wal;
    ASSERT_TRUE(wal.Open(config).has_value());
    for (int i = 0; i < 20; ++i) {
      const std::string payload = "missing_floor_" + std::to_string(i);
      ASSERT_TRUE(wal.Append(WalOpType::kVecSet, payload.data(), static_cast<uint32_t>(payload.size())).has_value());
    }
  }
  std::vector<fs::path> segments;
  for (const auto& entry : fs::directory_iterator(test_dir_)) {
    if (entry.path().extension() == ".log") {
      segments.push_back(entry.path());
    }
  }
  std::sort(segments.begin(), segments.end());
  ASSERT_GT(segments.size(), 1U);
  ASSERT_TRUE(fs::remove(segments.front()));

  WriteAheadLog recovered;
  ASSERT_TRUE(recovered.Open(config).has_value());
  auto replayed = recovered.Replay(1, [](const WalRecord&) {});
  ASSERT_FALSE(replayed.has_value());
  EXPECT_EQ(replayed.error().code(), utils::ErrorCode::kWalCorrupted);
}

TEST_F(WalTest, ReplayEmpty) {
  WriteAheadLog wal;
  ASSERT_TRUE(wal.Open(MakeConfig()));

  uint64_t count = 0;
  auto result = wal.Replay(1, [&](const WalRecord&) { ++count; });
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, 0U);
  EXPECT_EQ(count, 0U);
}

TEST_F(WalTest, ReplayPreservesTimestamp) {
  {
    WriteAheadLog wal;
    ASSERT_TRUE(wal.Open(MakeConfig()));
    std::string p = "data";
    ASSERT_TRUE(wal.Append(WalOpType::kVecSet, p.data(), static_cast<uint32_t>(p.size())));
  }

  {
    WriteAheadLog wal;
    ASSERT_TRUE(wal.Open(MakeConfig()));

    std::vector<WalRecord> records;
    wal.Replay(1, [&](const WalRecord& r) { records.push_back(r); });

    ASSERT_EQ(records.size(), 1U);
    EXPECT_GT(records[0].timestamp_us, 0U);
  }
}

// --- CRC Corruption ---

TEST_F(WalTest, CRCCorruptionDetected) {
  // Write a valid record
  {
    WriteAheadLog wal;
    ASSERT_TRUE(wal.Open(MakeConfig()));
    std::string p = "valid_data";
    ASSERT_TRUE(wal.Append(WalOpType::kVecSet, p.data(), static_cast<uint32_t>(p.size())));
  }

  // Corrupt the CRC field (bytes 4-7 after file header, in the first record)
  {
    std::string path = test_dir_ + "/wal-000001.log";
    int fd = ::open(path.c_str(), O_RDWR);
    ASSERT_GE(fd, 0);

    // File header is 8 bytes, then record: [length:4][crc:4][body...]
    // Corrupt the CRC at offset 8+4=12
    uint8_t garbage = 0xFF;
    ::lseek(fd, 12, SEEK_SET);
    ::write(fd, &garbage, 1);
    ::close(fd);
  }

  // Startup must fail closed rather than silently skipping the record.
  {
    WriteAheadLog wal;
    auto opened = wal.Open(MakeConfig());
    EXPECT_FALSE(opened.has_value());
  }
}

// --- File Rotation ---

TEST_F(WalTest, FileRotation) {
  // Use a very small max file size to force rotation
  auto config = MakeConfig(128);  // 128 bytes — will rotate quickly

  WriteAheadLog wal;
  ASSERT_TRUE(wal.Open(config));

  // Write enough records to trigger multiple rotations
  for (int i = 0; i < 20; ++i) {
    std::string p = "rotation_test_" + std::to_string(i);
    auto result = wal.Append(WalOpType::kVecSet, p.data(), static_cast<uint32_t>(p.size()));
    ASSERT_TRUE(result.has_value()) << result.error().to_string();
  }
  wal.Close();

  // Check multiple WAL files were created
  int file_count = 0;
  for (const auto& entry : fs::directory_iterator(test_dir_)) {
    if (entry.path().extension() == ".log") {
      ++file_count;
    }
  }
  EXPECT_GT(file_count, 1);

  // Verify all records can be replayed
  WriteAheadLog wal2;
  ASSERT_TRUE(wal2.Open(config));

  std::vector<WalRecord> records;
  auto result = wal2.Replay(1, [&](const WalRecord& r) { records.push_back(r); });
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, 20U);
  EXPECT_EQ(records.size(), 20U);

  // Verify sequence continuity
  for (size_t i = 0; i < records.size(); ++i) {
    EXPECT_EQ(records[i].sequence, i + 1);
  }
}

// --- Truncate ---

TEST_F(WalTest, TruncateRemovesOldFiles) {
  auto config = MakeConfig(128);  // Small files to force rotation

  WriteAheadLog wal;
  ASSERT_TRUE(wal.Open(config));

  // Write records across multiple files
  for (int i = 0; i < 20; ++i) {
    std::string p = "truncate_test_" + std::to_string(i);
    ASSERT_TRUE(wal.Append(WalOpType::kVecSet, p.data(), static_cast<uint32_t>(p.size())));
  }

  uint64_t mid_seq = 10;

  // Count files before truncate
  int files_before = 0;
  for (const auto& entry : fs::directory_iterator(test_dir_)) {
    if (entry.path().extension() == ".log")
      ++files_before;
  }

  // Truncate up to sequence 10
  auto result = wal.Truncate(mid_seq);
  ASSERT_TRUE(result.has_value());

  // Count files after truncate
  int files_after = 0;
  for (const auto& entry : fs::directory_iterator(test_dir_)) {
    if (entry.path().extension() == ".log")
      ++files_after;
  }

  EXPECT_LT(files_after, files_before);

  // Verify remaining records can still be replayed
  std::vector<WalRecord> records;
  wal.Replay(mid_seq + 1, [&](const WalRecord& r) { records.push_back(r); });
  EXPECT_GT(records.size(), 0U);
}

TEST_F(WalTest, TruncatePreservesCurrentFile) {
  WriteAheadLog wal;
  ASSERT_TRUE(wal.Open(MakeConfig()));

  std::string p = "keep_me";
  ASSERT_TRUE(wal.Append(WalOpType::kVecSet, p.data(), static_cast<uint32_t>(p.size())));

  // Truncate up to current sequence — current file should be preserved
  auto result = wal.Truncate(wal.CurrentSequence());
  ASSERT_TRUE(result.has_value());

  // File should still exist
  int file_count = 0;
  for (const auto& entry : fs::directory_iterator(test_dir_)) {
    if (entry.path().extension() == ".log")
      ++file_count;
  }
  EXPECT_GE(file_count, 1);
}

TEST_F(WalTest, TruncatePropagatesUnlinkFailure) {
  WriteAheadLog wal;
  ASSERT_TRUE(wal.Open(MakeConfig(128)));
  for (int i = 0; i < 20; ++i) {
    const std::string payload = "unlink_failure_" + std::to_string(i);
    ASSERT_TRUE(wal.Append(WalOpType::kVecSet, payload.data(), static_cast<uint32_t>(payload.size())).has_value());
  }

  std::vector<fs::path> segments;
  for (const auto& entry : fs::directory_iterator(test_dir_)) {
    if (entry.path().extension() == ".log") {
      segments.push_back(entry.path());
    }
  }
  std::sort(segments.begin(), segments.end());
  ASSERT_GT(segments.size(), 1U);
  // Simulate an external cleanup/race. The in-memory catalog still owns this
  // segment, so ENOENT must be reported rather than silently drifting.
  ASSERT_TRUE(fs::remove(segments.front()));

  auto result = wal.Truncate(wal.CurrentSequence());
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), utils::ErrorCode::kWalTruncateFailed);
}

// --- Incomplete Records ---

TEST_F(WalTest, LatestIncompleteRecordIsRepaired) {
  // Write valid records
  {
    WriteAheadLog wal;
    ASSERT_TRUE(wal.Open(MakeConfig()));
    std::string p1 = "complete_record";
    ASSERT_TRUE(wal.Append(WalOpType::kVecSet, p1.data(), static_cast<uint32_t>(p1.size())));
  }

  uintmax_t valid_size = 0;
  // Append garbage bytes to simulate crash mid-write
  {
    std::string path = test_dir_ + "/wal-000001.log";
    valid_size = fs::file_size(path);
    int fd = ::open(path.c_str(), O_WRONLY | O_APPEND);
    ASSERT_GE(fd, 0);

    // Write a partial record header (only 4 bytes of length, no CRC or body)
    uint32_t fake_length = 100;
    ::write(fd, &fake_length, sizeof(fake_length));
    ::close(fd);
  }

  // Replay should return only the valid record
  {
    WriteAheadLog wal;
    ASSERT_TRUE(wal.Open(MakeConfig()));
    EXPECT_EQ(fs::file_size(test_dir_ + "/wal-000001.log"), valid_size);

    std::vector<WalRecord> records;
    auto result = wal.Replay(1, [&](const WalRecord& r) { records.push_back(r); });
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 1U);
    EXPECT_EQ(records[0].sequence, 1U);
  }
}

TEST_F(WalTest, TornTailInNonLatestSegmentIsRejected) {
  const auto config = MakeConfig(96);
  {
    WriteAheadLog wal;
    ASSERT_TRUE(wal.Open(config));
    for (int index = 0; index < 12; ++index) {
      const std::string payload = "rotated_record_" + std::to_string(index);
      ASSERT_TRUE(wal.Append(WalOpType::kVecSet, payload.data(), static_cast<uint32_t>(payload.size())));
    }
  }

  const std::string first_segment = test_dir_ + "/wal-000001.log";
  ASSERT_TRUE(fs::exists(test_dir_ + "/wal-000002.log"));
  const auto original_size = fs::file_size(first_segment);
  ASSERT_GT(original_size, kWalFileHeaderSize);
  ASSERT_EQ(::truncate(first_segment.c_str(), static_cast<off_t>(original_size - 1)), 0);

  WriteAheadLog wal;
  EXPECT_FALSE(wal.Open(config).has_value());
}

TEST_F(WalTest, SequenceGapIsRejectedEvenWithValidCRC) {
  {
    WriteAheadLog wal;
    ASSERT_TRUE(wal.Open(MakeConfig()));
    const std::string payload = "sequence";
    ASSERT_TRUE(wal.Append(WalOpType::kVecSet, payload.data(), static_cast<uint32_t>(payload.size())));
    ASSERT_TRUE(wal.Append(WalOpType::kVecSet, payload.data(), static_cast<uint32_t>(payload.size())));
  }

  ASSERT_TRUE(RewriteRecordSequence(test_dir_ + "/wal-000001.log", 1, 3));
  WriteAheadLog wal;
  EXPECT_FALSE(wal.Open(MakeConfig()).has_value());
}

TEST_F(WalTest, DuplicateSequenceIsRejectedEvenWithValidCRC) {
  {
    WriteAheadLog wal;
    ASSERT_TRUE(wal.Open(MakeConfig()));
    const std::string payload = "sequence";
    ASSERT_TRUE(wal.Append(WalOpType::kVecSet, payload.data(), static_cast<uint32_t>(payload.size())));
    ASSERT_TRUE(wal.Append(WalOpType::kVecSet, payload.data(), static_cast<uint32_t>(payload.size())));
  }

  ASSERT_TRUE(RewriteRecordSequence(test_dir_ + "/wal-000001.log", 1, 1));
  WriteAheadLog wal;
  EXPECT_FALSE(wal.Open(MakeConfig()).has_value());
}

// --- All Operation Types ---

TEST_F(WalTest, AllOperationTypes) {
  {
    WriteAheadLog wal;
    ASSERT_TRUE(wal.Open(MakeConfig()));

    auto append = [&](WalOpType op, const std::string& p) {
      return wal.Append(op, p.data(), static_cast<uint32_t>(p.size()));
    };

    ASSERT_TRUE(append(WalOpType::kVecSet, "vec_set"));
    ASSERT_TRUE(append(WalOpType::kVecDel, "vec_del"));
    ASSERT_TRUE(append(WalOpType::kEventAdd, "event_add"));
    ASSERT_TRUE(append(WalOpType::kEventDel, "event_del"));
    ASSERT_TRUE(append(WalOpType::kMetaSet, "meta_set"));
    ASSERT_TRUE(append(WalOpType::kMetaDel, "meta_del"));
  }

  {
    WriteAheadLog wal;
    ASSERT_TRUE(wal.Open(MakeConfig()));

    std::vector<WalRecord> records;
    wal.Replay(1, [&](const WalRecord& r) { records.push_back(r); });

    ASSERT_EQ(records.size(), 6U);
    EXPECT_EQ(records[0].op, WalOpType::kVecSet);
    EXPECT_EQ(records[1].op, WalOpType::kVecDel);
    EXPECT_EQ(records[2].op, WalOpType::kEventAdd);
    EXPECT_EQ(records[3].op, WalOpType::kEventDel);
    EXPECT_EQ(records[4].op, WalOpType::kMetaSet);
    EXPECT_EQ(records[5].op, WalOpType::kMetaDel);
  }
}

// --- Empty Payload ---

TEST_F(WalTest, EmptyPayload) {
  {
    WriteAheadLog wal;
    ASSERT_TRUE(wal.Open(MakeConfig()));
    auto result = wal.Append(WalOpType::kVecDel, nullptr, 0);
    ASSERT_TRUE(result.has_value());
  }

  {
    WriteAheadLog wal;
    ASSERT_TRUE(wal.Open(MakeConfig()));

    std::vector<WalRecord> records;
    wal.Replay(1, [&](const WalRecord& r) { records.push_back(r); });

    ASSERT_EQ(records.size(), 1U);
    EXPECT_TRUE(records[0].payload.empty());
  }
}

// --- Large Payload ---

TEST_F(WalTest, LargePayload) {
  // Simulate a 768-dim float vector (3072 bytes)
  std::vector<float> vec(768, 0.5F);

  {
    WriteAheadLog wal;
    ASSERT_TRUE(wal.Open(MakeConfig()));
    auto result = wal.Append(WalOpType::kVecSet, vec.data(), static_cast<uint32_t>(vec.size() * sizeof(float)));
    ASSERT_TRUE(result.has_value());
  }

  {
    WriteAheadLog wal;
    ASSERT_TRUE(wal.Open(MakeConfig()));

    std::vector<WalRecord> records;
    wal.Replay(1, [&](const WalRecord& r) { records.push_back(r); });

    ASSERT_EQ(records.size(), 1U);
    EXPECT_EQ(records[0].payload.size(), 768 * sizeof(float));

    // Verify data integrity
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    const auto* data = reinterpret_cast<const float*>(records[0].payload.data());
    for (size_t i = 0; i < 768; ++i) {
      EXPECT_FLOAT_EQ(data[i], 0.5F);
    }
  }
}

// --- Sequence Continuity Across Reopen ---

TEST_F(WalTest, SequenceContinuityAcrossReopen) {
  {
    WriteAheadLog wal;
    ASSERT_TRUE(wal.Open(MakeConfig()));
    std::string p = "first";
    ASSERT_TRUE(wal.Append(WalOpType::kVecSet, p.data(), static_cast<uint32_t>(p.size())));
    ASSERT_TRUE(wal.Append(WalOpType::kVecSet, p.data(), static_cast<uint32_t>(p.size())));
    EXPECT_EQ(wal.CurrentSequence(), 2U);
  }

  {
    WriteAheadLog wal;
    ASSERT_TRUE(wal.Open(MakeConfig()));
    EXPECT_EQ(wal.CurrentSequence(), 2U);

    std::string p = "second";
    auto result = wal.Append(WalOpType::kVecSet, p.data(), static_cast<uint32_t>(p.size()));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 3U);
  }
}

// --- Batch Fsync ---

TEST_F(WalTest, BatchFsyncMode) {
  auto config = MakeConfig();
  config.sync_on_write = false;
  config.sync_interval_ms = 50;

  WriteAheadLog wal;
  ASSERT_TRUE(wal.Open(config));

  // Write several records
  for (int i = 0; i < 10; ++i) {
    std::string p = "batch_" + std::to_string(i);
    ASSERT_TRUE(wal.Append(WalOpType::kVecSet, p.data(), static_cast<uint32_t>(p.size())));
  }

  // Wait for batch sync to trigger
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  wal.Close();

  // Verify all records survive
  WriteAheadLog wal2;
  ASSERT_TRUE(wal2.Open(config));

  uint64_t count = 0;
  wal2.Replay(1, [&](const WalRecord&) { ++count; });
  EXPECT_EQ(count, 10U);
}

// --- Destructor Safety ---

TEST_F(WalTest, DestructorClosesCleanly) {
  {
    WriteAheadLog wal;
    ASSERT_TRUE(wal.Open(MakeConfig()));
    std::string p = "destructor_test";
    ASSERT_TRUE(wal.Append(WalOpType::kVecSet, p.data(), static_cast<uint32_t>(p.size())));
    // wal goes out of scope — destructor should close cleanly
  }

  // Verify record persisted
  WriteAheadLog wal;
  ASSERT_TRUE(wal.Open(MakeConfig()));
  uint64_t count = 0;
  wal.Replay(1, [&](const WalRecord&) { ++count; });
  EXPECT_EQ(count, 1U);
}

// --- Multiple Rotations with Replay ---

TEST_F(WalTest, MultipleRotationsReplayAll) {
  auto config = MakeConfig(96);  // Very small — triggers many rotations

  {
    WriteAheadLog wal;
    ASSERT_TRUE(wal.Open(config));

    for (int i = 0; i < 50; ++i) {
      std::string p = "rot_" + std::to_string(i);
      ASSERT_TRUE(wal.Append(WalOpType::kVecSet, p.data(), static_cast<uint32_t>(p.size())));
    }
  }

  {
    WriteAheadLog wal;
    ASSERT_TRUE(wal.Open(config));

    std::vector<uint64_t> sequences;
    wal.Replay(1, [&](const WalRecord& r) { sequences.push_back(r.sequence); });

    EXPECT_EQ(sequences.size(), 50U);
    // Verify monotonically increasing
    for (size_t i = 1; i < sequences.size(); ++i) {
      EXPECT_GT(sequences[i], sequences[i - 1]);
    }
  }
}

// --- WAL File Header Validation ---

TEST_F(WalTest, InvalidMagicRejected) {
  // Create a file with invalid magic
  std::string path = test_dir_ + "/wal-000001.log";
  {
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    ASSERT_GE(fd, 0);
    uint32_t bad_magic = 0xDEADBEEF;
    uint32_t version = kWalVersion;
    ::write(fd, &bad_magic, sizeof(bad_magic));
    ::write(fd, &version, sizeof(version));
    ::close(fd);
  }

  // WAL must fail closed on an invalid segment header.
  WriteAheadLog wal;
  EXPECT_FALSE(wal.Open(MakeConfig()).has_value());
}

TEST_F(WalTest, PartiallyNumericSegmentFilenameIsIgnored) {
  const auto config = MakeConfig();
  {
    WriteAheadLog wal;
    ASSERT_TRUE(wal.Open(config).has_value());
    const std::string payload = "canonical";
    ASSERT_TRUE(wal.Append(WalOpType::kVecSet, payload.data(), static_cast<uint32_t>(payload.size())).has_value());
  }
  fs::copy_file(test_dir_ + "/wal-000001.log", test_dir_ + "/wal-00001x.log");

  WriteAheadLog recovered;
  ASSERT_TRUE(recovered.Open(config).has_value());
  auto replayed = recovered.Replay(1, [](const WalRecord&) {});
  ASSERT_TRUE(replayed.has_value());
  EXPECT_EQ(*replayed, 1U);
}

TEST_F(WalTest, ReplayPropagatesCallbackFailure) {
  WriteAheadLog wal;
  ASSERT_TRUE(wal.Open(MakeConfig()));
  const std::string payload = "record";
  ASSERT_TRUE(wal.Append(WalOpType::kVecSet, payload.data(), static_cast<uint32_t>(payload.size())).has_value());

  auto replayed = wal.Replay(1, [](const WalRecord&) -> Expected<void, Error> {
    return utils::MakeUnexpected(utils::MakeError(utils::ErrorCode::kStorageCorrupted, "apply failed"));
  });

  ASSERT_FALSE(replayed.has_value());
  EXPECT_EQ(replayed.error().message(), "apply failed");
}

// --- Concurrent Append ---

TEST_F(WalTest, ConcurrentAppend) {
  WriteAheadLog wal;
  ASSERT_TRUE(wal.Open(MakeConfig()));

  constexpr int kThreads = 8;
  constexpr int kAppendsPerThread = 100;
  std::atomic<int> success_count{0};

  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&, t]() {
      for (int i = 0; i < kAppendsPerThread; ++i) {
        std::string p = "t" + std::to_string(t) + "_" + std::to_string(i);
        auto result = wal.Append(WalOpType::kVecSet, p.data(), static_cast<uint32_t>(p.size()));
        if (result.has_value()) {
          success_count++;
        }
      }
    });
  }

  for (auto& t : threads) {
    t.join();
  }

  EXPECT_EQ(success_count.load(), kThreads * kAppendsPerThread);
  EXPECT_EQ(wal.CurrentSequence(), static_cast<uint64_t>(kThreads * kAppendsPerThread));

  // Verify all records can be replayed
  uint64_t replayed = 0;
  auto result = wal.Replay(1, [&](const WalRecord&) { ++replayed; });
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, static_cast<uint64_t>(kThreads * kAppendsPerThread));
}

// --- Concurrent Append with Rotation ---

TEST_F(WalTest, ConcurrentAppendWithRotation) {
  auto config = MakeConfig(256);  // Small files to force rotation
  WriteAheadLog wal;
  ASSERT_TRUE(wal.Open(config));

  constexpr int kThreads = 4;
  constexpr int kAppendsPerThread = 50;
  std::atomic<int> success_count{0};

  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&, t]() {
      for (int i = 0; i < kAppendsPerThread; ++i) {
        std::string p = "rotate_t" + std::to_string(t) + "_" + std::to_string(i);
        auto result = wal.Append(WalOpType::kEventAdd, p.data(), static_cast<uint32_t>(p.size()));
        if (result.has_value()) {
          success_count++;
        }
      }
    });
  }

  for (auto& t : threads) {
    t.join();
  }

  EXPECT_EQ(success_count.load(), kThreads * kAppendsPerThread);
  wal.Close();

  // Verify all replayed across rotated files
  WriteAheadLog wal2;
  ASSERT_TRUE(wal2.Open(config));
  uint64_t replayed = 0;
  wal2.Replay(1, [&](const WalRecord&) { ++replayed; });
  EXPECT_EQ(replayed, static_cast<uint64_t>(kThreads * kAppendsPerThread));
}

}  // namespace
}  // namespace nvecd::storage
