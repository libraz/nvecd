/**
 * @file metadata_filter_test.cpp
 * @brief Tests for MetadataFilter and FilterCondition
 */

#include "vectors/metadata_filter.h"

#include <gtest/gtest.h>

namespace nvecd::vectors {
namespace {

// ============================================================================
// FilterCondition::Match
// ============================================================================

TEST(FilterConditionTest, EqString) {
  FilterCondition cond;
  cond.field = "status";
  cond.op = FilterOp::kEq;
  cond.value = std::string("active");

  EXPECT_TRUE(cond.Match(MetadataValue(std::string("active"))));
  EXPECT_FALSE(cond.Match(MetadataValue(std::string("inactive"))));
}

TEST(FilterConditionTest, EqInt) {
  FilterCondition cond;
  cond.field = "count";
  cond.op = FilterOp::kEq;
  cond.value = int64_t(42);

  EXPECT_TRUE(cond.Match(MetadataValue(int64_t(42))));
  EXPECT_FALSE(cond.Match(MetadataValue(int64_t(43))));
}

TEST(FilterConditionTest, EqDouble) {
  FilterCondition cond;
  cond.field = "price";
  cond.op = FilterOp::kEq;
  cond.value = 9.99;

  EXPECT_TRUE(cond.Match(MetadataValue(9.99)));
  EXPECT_FALSE(cond.Match(MetadataValue(10.0)));
}

TEST(FilterConditionTest, EqBool) {
  FilterCondition cond;
  cond.field = "active";
  cond.op = FilterOp::kEq;
  cond.value = true;

  EXPECT_TRUE(cond.Match(MetadataValue(true)));
  EXPECT_FALSE(cond.Match(MetadataValue(false)));
}

TEST(FilterConditionTest, NeString) {
  FilterCondition cond;
  cond.field = "status";
  cond.op = FilterOp::kNe;
  cond.value = std::string("deleted");

  EXPECT_TRUE(cond.Match(MetadataValue(std::string("active"))));
  EXPECT_FALSE(cond.Match(MetadataValue(std::string("deleted"))));
}

TEST(FilterConditionTest, GtInt) {
  FilterCondition cond;
  cond.field = "count";
  cond.op = FilterOp::kGt;
  cond.value = int64_t(10);

  EXPECT_TRUE(cond.Match(MetadataValue(int64_t(11))));
  EXPECT_FALSE(cond.Match(MetadataValue(int64_t(10))));
  EXPECT_FALSE(cond.Match(MetadataValue(int64_t(9))));
}

TEST(FilterConditionTest, GeDouble) {
  FilterCondition cond;
  cond.field = "price";
  cond.op = FilterOp::kGe;
  cond.value = 5.0;

  EXPECT_TRUE(cond.Match(MetadataValue(5.0)));
  EXPECT_TRUE(cond.Match(MetadataValue(5.1)));
  EXPECT_FALSE(cond.Match(MetadataValue(4.9)));
}

TEST(FilterConditionTest, LtInt) {
  FilterCondition cond;
  cond.field = "count";
  cond.op = FilterOp::kLt;
  cond.value = int64_t(100);

  EXPECT_TRUE(cond.Match(MetadataValue(int64_t(99))));
  EXPECT_FALSE(cond.Match(MetadataValue(int64_t(100))));
}

TEST(FilterConditionTest, LeDouble) {
  FilterCondition cond;
  cond.field = "price";
  cond.op = FilterOp::kLe;
  cond.value = 10.0;

  EXPECT_TRUE(cond.Match(MetadataValue(10.0)));
  EXPECT_TRUE(cond.Match(MetadataValue(9.99)));
  EXPECT_FALSE(cond.Match(MetadataValue(10.01)));
}

TEST(FilterConditionTest, InString) {
  FilterCondition cond;
  cond.field = "category";
  cond.op = FilterOp::kIn;
  cond.values = {MetadataValue(std::string("news")),
                 MetadataValue(std::string("blog"))};

  EXPECT_TRUE(cond.Match(MetadataValue(std::string("news"))));
  EXPECT_TRUE(cond.Match(MetadataValue(std::string("blog"))));
  EXPECT_FALSE(cond.Match(MetadataValue(std::string("video"))));
}

TEST(FilterConditionTest, NumericPromotion_IntVsDouble) {
  FilterCondition cond;
  cond.field = "price";
  cond.op = FilterOp::kGt;
  cond.value = 10.5;  // double

  // int64 compared against double
  EXPECT_TRUE(cond.Match(MetadataValue(int64_t(11))));
  EXPECT_FALSE(cond.Match(MetadataValue(int64_t(10))));
}

TEST(FilterConditionTest, TypeMismatch_StringVsInt) {
  FilterCondition cond;
  cond.field = "count";
  cond.op = FilterOp::kEq;
  cond.value = int64_t(42);

  // String vs int: should not match
  EXPECT_FALSE(cond.Match(MetadataValue(std::string("42"))));
}

// ============================================================================
// MetadataFilter::Match
// ============================================================================

TEST(MetadataFilterTest, EmptyFilterMatchesAnything) {
  MetadataFilter filter;
  Metadata meta = {{"status", std::string("active")}};
  EXPECT_TRUE(filter.Match(meta));
  EXPECT_TRUE(filter.Match(Metadata{}));
}

TEST(MetadataFilterTest, SingleCondition) {
  MetadataFilter filter;
  FilterCondition cond;
  cond.field = "status";
  cond.op = FilterOp::kEq;
  cond.value = std::string("active");
  filter.conditions.push_back(cond);

  Metadata meta1 = {{"status", std::string("active")}};
  Metadata meta2 = {{"status", std::string("deleted")}};
  Metadata meta3 = {};  // Missing field

  EXPECT_TRUE(filter.Match(meta1));
  EXPECT_FALSE(filter.Match(meta2));
  EXPECT_FALSE(filter.Match(meta3));
}

TEST(MetadataFilterTest, MultipleConditionsAND) {
  MetadataFilter filter;

  FilterCondition c1;
  c1.field = "status";
  c1.op = FilterOp::kEq;
  c1.value = std::string("active");
  filter.conditions.push_back(c1);

  FilterCondition c2;
  c2.field = "price";
  c2.op = FilterOp::kGt;
  c2.value = 10.0;
  filter.conditions.push_back(c2);

  Metadata meta_both = {{"status", std::string("active")}, {"price", 15.0}};
  Metadata meta_one = {{"status", std::string("active")}, {"price", 5.0}};
  Metadata meta_none = {{"status", std::string("deleted")}, {"price", 5.0}};

  EXPECT_TRUE(filter.Match(meta_both));
  EXPECT_FALSE(filter.Match(meta_one));
  EXPECT_FALSE(filter.Match(meta_none));
}

TEST(MetadataFilterTest, MissingFieldWithNe) {
  MetadataFilter filter;
  FilterCondition cond;
  cond.field = "status";
  cond.op = FilterOp::kNe;
  cond.value = std::string("deleted");
  filter.conditions.push_back(cond);

  // Missing field is treated as "not equal" for Ne operator
  Metadata meta_missing = {};
  EXPECT_TRUE(filter.Match(meta_missing));

  Metadata meta_deleted = {{"status", std::string("deleted")}};
  EXPECT_FALSE(filter.Match(meta_deleted));
}

}  // namespace
}  // namespace nvecd::vectors
