/**
 * @file wal_rotation_failure_test.cpp
 * @brief Tests that failed WAL writes leave no orphan segment and consume no sequence
 */

#include <gtest/gtest.h>
#include <signal.h>
#include <sys/resource.h>
#include <unistd.h>

#include <filesystem>
#include <string>

#include "storage/wal.h"

namespace nvecd::storage {
namespace {

namespace fs = std::filesystem;

/// Makes every write that grows a file fail with EFBIG for the guard's lifetime.
class WriteLimitGuard {
 public:
  WriteLimitGuard() {
    previous_handler_ = ::signal(SIGXFSZ, SIG_IGN);
    ::getrlimit(RLIMIT_FSIZE, &previous_limit_);
    rlimit limit = previous_limit_;
    limit.rlim_cur = 0;
    ::setrlimit(RLIMIT_FSIZE, &limit);
  }
  ~WriteLimitGuard() {
    ::setrlimit(RLIMIT_FSIZE, &previous_limit_);
    ::signal(SIGXFSZ, previous_handler_);
  }
  WriteLimitGuard(const WriteLimitGuard&) = delete;
  WriteLimitGuard& operator=(const WriteLimitGuard&) = delete;

 private:
  rlimit previous_limit_{};
  void (*previous_handler_)(int) = SIG_DFL;
};

class WalRotationFailureTest : public ::testing::Test {
 protected:
  void SetUp() override {
    test_dir_ = "/tmp/nvecd_wal_rotation_failure_test_" + std::to_string(::getpid());
    fs::remove_all(test_dir_);
    fs::create_directories(test_dir_);
  }

  void TearDown() override { fs::remove_all(test_dir_); }

  WriteAheadLog::Config MakeConfig() const {
    WriteAheadLog::Config config;
    config.directory = test_dir_;
    config.sync_on_write = true;
    return config;
  }

  size_t SegmentCount() const {
    size_t count = 0;
    for (const auto& entry : fs::directory_iterator(test_dir_)) {
      count += entry.path().extension() == ".log" ? 1 : 0;
    }
    return count;
  }

  std::string test_dir_;
};

TEST_F(WalRotationFailureTest, FailedHeaderWriteRemovesNewSegment) {
  WriteAheadLog wal;
  {
    WriteLimitGuard guard;
    EXPECT_FALSE(wal.Open(MakeConfig()).has_value());
  }
  EXPECT_FALSE(wal.IsOpen());
  EXPECT_EQ(SegmentCount(), 0U);
}

TEST_F(WalRotationFailureTest, OpenSucceedsAfterFailedHeaderWrite) {
  WriteAheadLog wal;
  {
    WriteLimitGuard guard;
    ASSERT_FALSE(wal.Open(MakeConfig()).has_value());
  }
  auto reopened = wal.Open(MakeConfig());
  ASSERT_TRUE(reopened.has_value()) << reopened.error().to_string();
  EXPECT_TRUE(wal.Append(WalOpType::kEventAdd, nullptr, 0).has_value());
  EXPECT_EQ(SegmentCount(), 1U);
}

TEST_F(WalRotationFailureTest, FailedAppendConsumesNoSequenceACheckpointCanCapture) {
  uint64_t checkpoint = 0;
  {
    WriteAheadLog wal;
    ASSERT_TRUE(wal.Open(MakeConfig()).has_value());
    ASSERT_TRUE(wal.Append(WalOpType::kEventAdd, nullptr, 0).has_value());
    ASSERT_TRUE(wal.Append(WalOpType::kEventAdd, nullptr, 0).has_value());
    {
      WriteLimitGuard guard;
      EXPECT_FALSE(wal.Append(WalOpType::kEventAdd, nullptr, 0).has_value());
    }
    // A snapshot taken now records this as its checkpoint.
    checkpoint = wal.CurrentSequence();
    EXPECT_EQ(checkpoint, 2U);
    wal.Close();
  }

  WriteAheadLog reopened;
  auto opened = reopened.Open(MakeConfig(), checkpoint);
  ASSERT_TRUE(opened.has_value()) << opened.error().to_string();
  auto next = reopened.Append(WalOpType::kEventAdd, nullptr, 0);
  ASSERT_TRUE(next.has_value()) << next.error().to_string();
  EXPECT_EQ(*next, checkpoint + 1);
}

}  // namespace
}  // namespace nvecd::storage
