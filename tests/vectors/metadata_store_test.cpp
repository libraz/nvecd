/**
 * @file metadata_store_test.cpp
 * @brief Tests for MetadataStore
 */

#include "vectors/metadata_store.h"

#include <gtest/gtest.h>

#include <algorithm>

namespace nvecd::vectors {
namespace {

class MetadataStoreTest : public ::testing::Test {
 protected:
  MetadataStore store_;
};

TEST_F(MetadataStoreTest, SetAndGet) {
  Metadata meta = {{"status", std::string("active")}, {"count", int64_t(42)}};
  store_.Set("item0", meta);

  const auto* result = store_.Get("item0");
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(std::get<std::string>(result->at("status")), "active");
  EXPECT_EQ(std::get<int64_t>(result->at("count")), 42);
}

TEST_F(MetadataStoreTest, GetNonExistent) {
  EXPECT_EQ(store_.Get("item0"), nullptr);
  EXPECT_EQ(store_.Get("missing"), nullptr);
}

TEST_F(MetadataStoreTest, Delete) {
  store_.Set("item0", {{"key", std::string("value")}});
  EXPECT_NE(store_.Get("item0"), nullptr);

  store_.Delete("item0");
  EXPECT_EQ(store_.Get("item0"), nullptr);
}

TEST_F(MetadataStoreTest, DeleteNonExistent) {
  store_.Delete("missing");  // Should not crash
}

TEST_F(MetadataStoreTest, Update) {
  store_.Set("item0", {{"status", std::string("draft")}});
  store_.Set("item0", {{"status", std::string("published")}});

  const auto* result = store_.Get("item0");
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(std::get<std::string>(result->at("status")), "published");
}

TEST_F(MetadataStoreTest, SetEmptyIsPresentButEmpty) {
  store_.Set("item0", {{"status", std::string("active")}});
  EXPECT_NE(store_.Get("item0"), nullptr);

  // Setting an empty metadata map keeps the item present with an empty value,
  // distinct from an item that was never set. Removal requires Delete().
  store_.Set("item0", {});
  const auto* result = store_.Get("item0");
  ASSERT_NE(result, nullptr);
  EXPECT_TRUE(result->empty());
  EXPECT_EQ(store_.Size(), 1U);

  // A never-set item reports as absent.
  EXPECT_EQ(store_.Get("never_set"), nullptr);

  // Delete removes presence.
  store_.Delete("item0");
  EXPECT_EQ(store_.Get("item0"), nullptr);
  EXPECT_EQ(store_.Size(), 0U);
}

TEST_F(MetadataStoreTest, EmptyEntryParticipatesInEmptyFilter) {
  store_.Set("empty", {});
  store_.Set("full", {{"status", std::string("active")}});

  // An empty (no-condition) filter returns all present items, including the
  // explicitly empty one.
  MetadataFilter empty_filter;
  auto all = store_.Filter(empty_filter);
  EXPECT_EQ(all.size(), 2U);

  // A non-empty filter never matches an empty metadata map.
  MetadataFilter filter;
  FilterCondition cond;
  cond.field = "status";
  cond.op = FilterOp::kEq;
  cond.value = std::string("active");
  filter.conditions.push_back(cond);

  auto matched = store_.Filter(filter);
  ASSERT_EQ(matched.size(), 1U);
  EXPECT_EQ(matched[0], "full");
  EXPECT_FALSE(store_.Matches("empty", filter));
  EXPECT_TRUE(store_.Matches("full", filter));

  // An empty entry still matches the empty filter via Matches().
  EXPECT_TRUE(store_.Matches("empty", empty_filter));
}

TEST_F(MetadataStoreTest, Size) {
  EXPECT_EQ(store_.Size(), 0U);

  store_.Set("a", {{"a", std::string("1")}});
  store_.Set("b", {{"b", std::string("2")}});
  EXPECT_EQ(store_.Size(), 2U);

  store_.Delete("a");
  EXPECT_EQ(store_.Size(), 1U);
}

TEST_F(MetadataStoreTest, Clear) {
  store_.Set("a", {{"a", std::string("1")}});
  store_.Set("b", {{"b", std::string("2")}});
  store_.Clear();
  EXPECT_EQ(store_.Size(), 0U);
  EXPECT_EQ(store_.Get("a"), nullptr);
}

TEST_F(MetadataStoreTest, FilterEmpty) {
  store_.Set("a", {{"status", std::string("active")}});
  store_.Set("b", {{"status", std::string("deleted")}});

  MetadataFilter filter;  // Empty filter
  auto result = store_.Filter(filter);
  EXPECT_EQ(result.size(), 2U);
}

TEST_F(MetadataStoreTest, FilterSingleCondition) {
  store_.Set("a", {{"status", std::string("active")}});
  store_.Set("b", {{"status", std::string("deleted")}});
  store_.Set("c", {{"status", std::string("active")}});

  MetadataFilter filter;
  FilterCondition cond;
  cond.field = "status";
  cond.op = FilterOp::kEq;
  cond.value = std::string("active");
  filter.conditions.push_back(cond);

  auto result = store_.Filter(filter);
  ASSERT_EQ(result.size(), 2U);
  std::sort(result.begin(), result.end());
  EXPECT_EQ(result[0], "a");
  EXPECT_EQ(result[1], "c");
}

TEST_F(MetadataStoreTest, FilterWithCandidates) {
  store_.Set("a", {{"status", std::string("active")}});
  store_.Set("b", {{"status", std::string("active")}});
  store_.Set("c", {{"status", std::string("active")}});

  MetadataFilter filter;
  FilterCondition cond;
  cond.field = "status";
  cond.op = FilterOp::kEq;
  cond.value = std::string("active");
  filter.conditions.push_back(cond);

  // Only check candidates {a, c}
  std::vector<std::string> candidates = {"a", "c"};
  auto result = store_.Filter(filter, candidates);
  ASSERT_EQ(result.size(), 2U);
  std::sort(result.begin(), result.end());
  EXPECT_EQ(result[0], "a");
  EXPECT_EQ(result[1], "c");
}

TEST_F(MetadataStoreTest, MatchesSingle) {
  store_.Set("item0", {{"status", std::string("active")}, {"price", 9.99}});

  MetadataFilter filter;
  FilterCondition cond;
  cond.field = "status";
  cond.op = FilterOp::kEq;
  cond.value = std::string("active");
  filter.conditions.push_back(cond);

  EXPECT_TRUE(store_.Matches("item0", filter));
  EXPECT_FALSE(store_.Matches("missing", filter));  // Non-existent

  MetadataFilter empty;
  EXPECT_TRUE(store_.Matches("item0", empty));
  EXPECT_TRUE(store_.Matches("missing", empty));  // Empty filter always matches
}

TEST_F(MetadataStoreTest, MultipleIds) {
  store_.Set("alpha", {{"a", std::string("x")}});
  store_.Set("beta", {{"b", std::string("y")}});
  store_.Set("gamma", {{"c", std::string("z")}});

  EXPECT_NE(store_.Get("alpha"), nullptr);
  EXPECT_NE(store_.Get("beta"), nullptr);
  EXPECT_NE(store_.Get("gamma"), nullptr);
  EXPECT_EQ(store_.Get("delta"), nullptr);
  EXPECT_EQ(store_.Size(), 3U);
}

}  // namespace
}  // namespace nvecd::vectors
