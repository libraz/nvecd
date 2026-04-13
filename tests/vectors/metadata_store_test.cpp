/**
 * @file metadata_store_test.cpp
 * @brief Tests for MetadataStore
 */

#include "vectors/metadata_store.h"

#include <gtest/gtest.h>

namespace nvecd::vectors {
namespace {

class MetadataStoreTest : public ::testing::Test {
 protected:
  MetadataStore store_;
};

TEST_F(MetadataStoreTest, SetAndGet) {
  Metadata meta = {{"status", std::string("active")}, {"count", int64_t(42)}};
  store_.Set(0, meta);

  const auto* result = store_.Get(0);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(std::get<std::string>(result->at("status")), "active");
  EXPECT_EQ(std::get<int64_t>(result->at("count")), 42);
}

TEST_F(MetadataStoreTest, GetNonExistent) {
  EXPECT_EQ(store_.Get(0), nullptr);
  EXPECT_EQ(store_.Get(999), nullptr);
}

TEST_F(MetadataStoreTest, Delete) {
  store_.Set(0, {{"key", std::string("value")}});
  EXPECT_NE(store_.Get(0), nullptr);

  store_.Delete(0);
  EXPECT_EQ(store_.Get(0), nullptr);
}

TEST_F(MetadataStoreTest, DeleteNonExistent) {
  store_.Delete(999);  // Should not crash
}

TEST_F(MetadataStoreTest, Update) {
  store_.Set(0, {{"status", std::string("draft")}});
  store_.Set(0, {{"status", std::string("published")}});

  const auto* result = store_.Get(0);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(std::get<std::string>(result->at("status")), "published");
}

TEST_F(MetadataStoreTest, Size) {
  EXPECT_EQ(store_.Size(), 0U);

  store_.Set(0, {{"a", std::string("1")}});
  store_.Set(5, {{"b", std::string("2")}});
  EXPECT_EQ(store_.Size(), 2U);

  store_.Delete(0);
  EXPECT_EQ(store_.Size(), 1U);
}

TEST_F(MetadataStoreTest, Clear) {
  store_.Set(0, {{"a", std::string("1")}});
  store_.Set(1, {{"b", std::string("2")}});
  store_.Clear();
  EXPECT_EQ(store_.Size(), 0U);
  EXPECT_EQ(store_.Get(0), nullptr);
}

TEST_F(MetadataStoreTest, FilterEmpty) {
  store_.Set(0, {{"status", std::string("active")}});
  store_.Set(1, {{"status", std::string("deleted")}});

  MetadataFilter filter;  // Empty filter
  auto result = store_.Filter(filter);
  EXPECT_EQ(result.size(), 2U);
}

TEST_F(MetadataStoreTest, FilterSingleCondition) {
  store_.Set(0, {{"status", std::string("active")}});
  store_.Set(1, {{"status", std::string("deleted")}});
  store_.Set(2, {{"status", std::string("active")}});

  MetadataFilter filter;
  FilterCondition cond;
  cond.field = "status";
  cond.op = FilterOp::kEq;
  cond.value = std::string("active");
  filter.conditions.push_back(cond);

  auto result = store_.Filter(filter);
  ASSERT_EQ(result.size(), 2U);
  EXPECT_EQ(result[0], 0U);
  EXPECT_EQ(result[1], 2U);
}

TEST_F(MetadataStoreTest, FilterWithCandidates) {
  store_.Set(0, {{"status", std::string("active")}});
  store_.Set(1, {{"status", std::string("active")}});
  store_.Set(2, {{"status", std::string("active")}});

  MetadataFilter filter;
  FilterCondition cond;
  cond.field = "status";
  cond.op = FilterOp::kEq;
  cond.value = std::string("active");
  filter.conditions.push_back(cond);

  // Only check candidates {0, 2}
  std::vector<uint32_t> candidates = {0, 2};
  auto result = store_.Filter(filter, candidates);
  ASSERT_EQ(result.size(), 2U);
  EXPECT_EQ(result[0], 0U);
  EXPECT_EQ(result[1], 2U);
}

TEST_F(MetadataStoreTest, MatchesSingle) {
  store_.Set(0, {{"status", std::string("active")}, {"price", 9.99}});

  MetadataFilter filter;
  FilterCondition cond;
  cond.field = "status";
  cond.op = FilterOp::kEq;
  cond.value = std::string("active");
  filter.conditions.push_back(cond);

  EXPECT_TRUE(store_.Matches(0, filter));
  EXPECT_FALSE(store_.Matches(1, filter));  // Non-existent

  MetadataFilter empty;
  EXPECT_TRUE(store_.Matches(0, empty));
  EXPECT_TRUE(store_.Matches(999, empty));  // Empty filter always matches
}

TEST_F(MetadataStoreTest, SparseIndices) {
  // Non-contiguous compact indices
  store_.Set(0, {{"a", std::string("x")}});
  store_.Set(100, {{"b", std::string("y")}});
  store_.Set(50, {{"c", std::string("z")}});

  EXPECT_NE(store_.Get(0), nullptr);
  EXPECT_NE(store_.Get(50), nullptr);
  EXPECT_NE(store_.Get(100), nullptr);
  EXPECT_EQ(store_.Get(25), nullptr);
  EXPECT_EQ(store_.Size(), 3U);
}

}  // namespace
}  // namespace nvecd::vectors
