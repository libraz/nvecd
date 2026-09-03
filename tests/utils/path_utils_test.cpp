/**
 * @file path_utils_test.cpp
 * @brief Unit tests for ValidateDumpPath
 */

#include "utils/path_utils.h"

#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>

#include <filesystem>

using namespace nvecd::utils;

class PathUtilsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // Give every case its own directory. Cases run as separate processes under
    // a parallel test run, so a shared fixed path lets one case's TearDown()
    // delete the directory another case is still working in.
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    test_dir_ =
        std::filesystem::temp_directory_path() / ("nvecd_path_test_" + std::to_string(::getpid()) + "_" + info->name());
    // A previous run killed mid-test can leave the directory behind with the
    // relaxed permissions one of the cases sets; start from a known state.
    std::filesystem::remove_all(test_dir_);
    std::filesystem::create_directories(test_dir_);
  }

  void TearDown() override { std::filesystem::remove_all(test_dir_); }

  std::filesystem::path test_dir_;
};

TEST_F(PathUtilsTest, ValidRelativePath) {
  auto result = ValidateDumpPath("snapshot.dmp", test_dir_.string());
  ASSERT_TRUE(result.has_value());
  EXPECT_TRUE(result->find("snapshot.dmp") != std::string::npos);
}

TEST_F(PathUtilsTest, PathTraversalRejected) {
  auto result = ValidateDumpPath("../etc/passwd", test_dir_.string());
  EXPECT_FALSE(result.has_value());
}

TEST_F(PathUtilsTest, AbsolutePathWithinDumpDir) {
  // Absolute path that is inside the dump dir should succeed
  std::string abs_path = (test_dir_ / "valid.dmp").string();
  auto result = ValidateDumpPath(abs_path, test_dir_.string());
  ASSERT_TRUE(result.has_value());
  EXPECT_TRUE(result->find("valid.dmp") != std::string::npos);
}

TEST_F(PathUtilsTest, AbsolutePathOutsideDumpDir) {
  auto result = ValidateDumpPath("/tmp/outside.dmp", test_dir_.string());
  EXPECT_FALSE(result.has_value());
}

TEST_F(PathUtilsTest, NonExistentDumpDir) {
  auto result = ValidateDumpPath("file.dmp", "/nonexistent/dir");
  EXPECT_FALSE(result.has_value());
}

TEST_F(PathUtilsTest, EmptyFilepathRejected) {
  // An empty filepath names no file, so it is refused before any path is built
  // from it.
  auto result = ValidateDumpPath("", test_dir_.string());
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), ErrorCode::kInvalidArgument);
  EXPECT_NE(result.error().message().find("must not be empty"), std::string::npos) << result.error().message();
}

TEST_F(PathUtilsTest, EmptyFilepathRejectedFromWorkingDirectoryInsideDumpDir) {
  // The refusal must follow from the input, not from where the process runs.
  // An empty path is never joined to dump_dir, and canonicalizing it yields the
  // working directory, so a containment-only check would accept an empty path
  // from a service started in its own data directory -- an ordinary way to run
  // a daemon -- and hand back the dump directory itself as a writable target.
  const auto previous_directory = std::filesystem::current_path();
  ASSERT_EQ(::chdir(test_dir_.c_str()), 0);
  auto result = ValidateDumpPath("", test_dir_.string());
  ASSERT_EQ(::chdir(previous_directory.c_str()), 0);

  ASSERT_FALSE(result.has_value()) << "empty filepath accepted, resolving to " << result.value();
  EXPECT_EQ(result.error().code(), ErrorCode::kInvalidArgument);
  EXPECT_NE(result.error().message().find("must not be empty"), std::string::npos) << result.error().message();
}

TEST_F(PathUtilsTest, NestedRelativePath) {
  // Create a subdirectory so weakly_canonical can resolve through it
  std::filesystem::create_directories(test_dir_ / "subdir");
  auto result = ValidateDumpPath("subdir/file.dmp", test_dir_.string());
  ASSERT_TRUE(result.has_value());
  EXPECT_TRUE(result->find("subdir") != std::string::npos);
  EXPECT_TRUE(result->find("file.dmp") != std::string::npos);
}

TEST_F(PathUtilsTest, DoubleDotsInFilenameRejected) {
  // Even if ".." is part of a filename, it is rejected as defense-in-depth
  auto result = ValidateDumpPath("my..file.dmp", test_dir_.string());
  EXPECT_FALSE(result.has_value());
}

TEST_F(PathUtilsTest, GroupWritableDumpDirectoryRejected) {
  ASSERT_EQ(::chmod(test_dir_.c_str(), 0770), 0);
  auto result = ValidateDumpPath("snapshot.dmp", test_dir_.string());
  EXPECT_FALSE(result.has_value());
}

TEST_F(PathUtilsTest, GroupWritableNestedDirectoryRejected) {
  const auto nested = test_dir_ / "nested";
  ASSERT_TRUE(std::filesystem::create_directory(nested));
  ASSERT_EQ(::chmod(nested.c_str(), 0770), 0);

  auto result = ValidateDumpPath("nested/snapshot.dmp", test_dir_.string());
  EXPECT_FALSE(result.has_value());
}

TEST_F(PathUtilsTest, ResolvesStoragePathThroughPrivateSymlink) {
  const auto symlink_path = test_dir_.parent_path() / ("nvecd_path_link_" + std::to_string(::getpid()));
  std::error_code error;
  std::filesystem::create_directory_symlink(test_dir_, symlink_path, error);
  ASSERT_FALSE(error) << error.message();

  auto result = ResolvePrivateStoragePath(symlink_path / "snapshot.dmp");
  ASSERT_TRUE(result.has_value()) << result.error().message();
  EXPECT_EQ(result->parent_path(), std::filesystem::canonical(test_dir_));

  std::filesystem::remove(symlink_path, error);
}

TEST_F(PathUtilsTest, DirectoryDescriptorPreventsAncestorRenameRedirection) {
  const auto original = test_dir_ / "private";
  const auto moved = test_dir_ / "private-moved";
  ASSERT_TRUE(std::filesystem::create_directory(original));
  ASSERT_EQ(::chmod(original.c_str(), 0700), 0);

  auto target_result = PrivateStorageTarget::Open(original / "snapshot.dmp");
  ASSERT_TRUE(target_result.has_value()) << target_result.error().message();
  auto target = std::move(target_result.value());

  std::filesystem::rename(original, moved);
  ASSERT_TRUE(std::filesystem::create_directory(original));
  ASSERT_EQ(::chmod(original.c_str(), 0700), 0);

  auto temporary_result = target.CreateTemporaryFile();
  ASSERT_TRUE(temporary_result.has_value()) << temporary_result.error().message();
  auto temporary = std::move(temporary_result.value());
  constexpr char contents[] = "trusted";
  ASSERT_EQ(::write(temporary.Get(), contents, sizeof(contents) - 1), static_cast<ssize_t>(sizeof(contents) - 1));
  ASSERT_TRUE(target.Publish(temporary).has_value());

  EXPECT_TRUE(std::filesystem::exists(moved / "snapshot.dmp"));
  EXPECT_FALSE(std::filesystem::exists(original / "snapshot.dmp"));
}
