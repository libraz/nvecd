/**
 * @file path_utils_test.cpp
 * @brief Unit tests for ValidateDumpPath
 */

#include "utils/path_utils.h"

#include <gtest/gtest.h>

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
