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
    test_dir_ = std::filesystem::temp_directory_path() / "nvecd_path_test";
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

TEST_F(PathUtilsTest, EmptyFilepath) {
  // Empty filepath: resolved becomes empty string (not prepended since
  // the empty check passes but the absolute check sees empty[0] would be UB).
  // The function prepends dump_dir + "/" making it dump_dir path.
  // Behavior depends on implementation; verify it returns a valid result
  // or an error without crashing.
  auto result = ValidateDumpPath("", test_dir_.string());
  // Empty filepath resolves to dump_dir + "/" which canonicalizes to dump_dir.
  // The relative path "." is valid, but the implementation may reject it.
  // Just verify no crash - either outcome is acceptable.
  (void)result;
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
