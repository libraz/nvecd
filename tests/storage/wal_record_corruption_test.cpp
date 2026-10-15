/**
 * @file wal_record_corruption_test.cpp
 * @brief Recovery (Open) and Replay must report the same error code for the same damaged record
 */

#include <fcntl.h>
#include <gtest/gtest.h>
#include <unistd.h>
#include <zlib.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "storage/wal.h"

namespace nvecd::storage {
namespace {

namespace fs = std::filesystem;

constexpr off_t kRecordFrameBytes = 8;  // length(4) + crc32(4)

bool PatchBytes(const std::string& path, off_t offset, const void* data, size_t size) {
  const int fd = ::open(path.c_str(), O_WRONLY);
  if (fd < 0) {
    return false;
  }
  const bool written = ::pwrite(fd, data, size, offset) == static_cast<ssize_t>(size);
  return ::close(fd) == 0 && written;
}

/// Rewrite the sequence of the first record in a segment and refresh its CRC32.
bool RewriteFirstSequence(const std::string& path, uint64_t sequence) {
  const int fd = ::open(path.c_str(), O_RDWR);
  if (fd < 0) {
    return false;
  }
  uint8_t frame[kRecordFrameBytes]{};
  bool ok = ::pread(fd, frame, sizeof(frame), kWalFileHeaderSize) == static_cast<ssize_t>(sizeof(frame));
  uint32_t body_length = 0;
  std::memcpy(&body_length, frame, sizeof(body_length));
  std::vector<uint8_t> body(body_length);
  ok = ok && ::pread(fd, body.data(), body.size(), kWalFileHeaderSize + kRecordFrameBytes) ==
                 static_cast<ssize_t>(body.size());
  if (ok) {
    std::memcpy(body.data(), &sequence, sizeof(sequence));
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    const auto crc = static_cast<uint32_t>(crc32(0L, reinterpret_cast<const Bytef*>(body.data()), body.size()));
    std::memcpy(frame + sizeof(uint32_t), &crc, sizeof(crc));
    ok = ::pwrite(fd, body.data(), body.size(), kWalFileHeaderSize + kRecordFrameBytes) ==
             static_cast<ssize_t>(body.size()) &&
         ::pwrite(fd, frame, sizeof(frame), kWalFileHeaderSize) == static_cast<ssize_t>(sizeof(frame));
  }
  return ::close(fd) == 0 && ok;
}

class WalRecordCorruptionTest : public ::testing::Test {
 protected:
  void SetUp() override {
    test_dir_ = "/tmp/nvecd_wal_record_corruption_test_" + std::to_string(::getpid());
    fs::remove_all(test_dir_);
    fs::create_directories(test_dir_);
  }

  void TearDown() override { fs::remove_all(test_dir_); }

  WriteAheadLog::Config MakeConfig(uint64_t max_file_size = 64 * 1024 * 1024) const {
    WriteAheadLog::Config config;
    config.directory = test_dir_;
    config.max_file_size = max_file_size;
    config.sync_on_write = true;
    return config;
  }

  std::string Segment(int number) const {
    char name[32];
    std::snprintf(name, sizeof(name), "/wal-%06d.log", number);
    return test_dir_ + name;
  }

  /// Error code Replay reports on an open log, then the code a fresh Open reports on the same files.
  void ExpectSameCode(WriteAheadLog& wal, utils::ErrorCode expected) {
    const auto replayed = wal.Replay(0, [](const WalRecord&) {});
    ASSERT_FALSE(replayed.has_value());
    EXPECT_EQ(replayed.error().code(), expected) << "Replay: " << replayed.error().message();
    wal.Close();

    WriteAheadLog recovered;
    const auto opened = recovered.Open(MakeConfig());
    ASSERT_FALSE(opened.has_value());
    EXPECT_EQ(opened.error().code(), expected) << "Open: " << opened.error().message();
  }

  void AppendRecords(WriteAheadLog& wal, int count) {
    const std::string payload = "payload";
    for (int i = 0; i < count; ++i) {
      ASSERT_TRUE(wal.Append(WalOpType::kEventAdd, payload.data(), static_cast<uint32_t>(payload.size())));
    }
  }

  std::string test_dir_;
};

TEST_F(WalRecordCorruptionTest, InvalidRecordLengthIsReportedAlike) {
  WriteAheadLog wal;
  ASSERT_TRUE(wal.Open(MakeConfig()));
  AppendRecords(wal, 1);
  const uint32_t bogus_length = kMaxWalRecordBodySize + 1;
  ASSERT_TRUE(PatchBytes(Segment(1), kWalFileHeaderSize, &bogus_length, sizeof(bogus_length)));

  ExpectSameCode(wal, utils::ErrorCode::kWalCorrupted);
}

TEST_F(WalRecordCorruptionTest, SequenceZeroIsReportedAlike) {
  WriteAheadLog wal;
  ASSERT_TRUE(wal.Open(MakeConfig()));
  AppendRecords(wal, 1);
  ASSERT_TRUE(RewriteFirstSequence(Segment(1), 0));

  ExpectSameCode(wal, utils::ErrorCode::kWalCorrupted);
}

TEST_F(WalRecordCorruptionTest, SequenceGapAcrossSegmentsIsReportedAlike) {
  WriteAheadLog wal;
  // A tiny segment limit puts each record in its own segment.
  ASSERT_TRUE(wal.Open(MakeConfig(/*max_file_size=*/1)));
  AppendRecords(wal, 2);
  ASSERT_TRUE(fs::exists(Segment(2)));
  ASSERT_TRUE(RewriteFirstSequence(Segment(2), 9));

  ExpectSameCode(wal, utils::ErrorCode::kWalCorrupted);
}

TEST_F(WalRecordCorruptionTest, TornBodyInOlderSegmentIsReportedAlike) {
  WriteAheadLog wal;
  ASSERT_TRUE(wal.Open(MakeConfig(/*max_file_size=*/1)));
  AppendRecords(wal, 2);
  ASSERT_TRUE(fs::exists(Segment(2)));
  fs::resize_file(Segment(1), fs::file_size(Segment(1)) - 1);

  ExpectSameCode(wal, utils::ErrorCode::kWalCorrupted);
}

TEST_F(WalRecordCorruptionTest, ChecksumMismatchIsReportedAlike) {
  WriteAheadLog wal;
  ASSERT_TRUE(wal.Open(MakeConfig()));
  AppendRecords(wal, 1);
  const uint8_t flipped = 0xFF;
  ASSERT_TRUE(PatchBytes(Segment(1), kWalFileHeaderSize + kRecordFrameBytes + kWalRecordHeaderSize, &flipped, 1));

  ExpectSameCode(wal, utils::ErrorCode::kWalCRCMismatch);
}

}  // namespace
}  // namespace nvecd::storage
