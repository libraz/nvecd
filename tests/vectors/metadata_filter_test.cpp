/**
 * @file metadata_filter_test.cpp
 * @brief Tests for MetadataFilter and FilterCondition
 */

#include "vectors/metadata_filter.h"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <limits>

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

TEST(FilterConditionTest, NanNeverComparesEqualOrOrdered) {
  // An older snapshot or WAL may still hold a NaN metadata value.
  const MetadataValue stored(std::numeric_limits<double>::quiet_NaN());
  for (const FilterOp op : {FilterOp::kEq, FilterOp::kNe, FilterOp::kGt, FilterOp::kGe, FilterOp::kLt, FilterOp::kLe}) {
    FilterCondition by_double;
    by_double.field = "price";
    by_double.op = op;
    by_double.value = 10.0;
    EXPECT_FALSE(by_double.Match(stored)) << static_cast<int>(op);

    FilterCondition by_int = by_double;
    by_int.value = int64_t{10};
    EXPECT_FALSE(by_int.Match(stored)) << static_cast<int>(op);

    FilterCondition stored_int = by_double;
    stored_int.value = std::numeric_limits<double>::quiet_NaN();
    EXPECT_FALSE(stored_int.Match(MetadataValue(int64_t{10}))) << static_cast<int>(op);
  }
}

TEST(FilterConditionTest, MixedInt64AndDoubleComparisonsCoverIntegerBoundaries) {
  struct ComparisonCase {
    int64_t integer;
    double real;
    int comparison;
    const char* description;
  };

  constexpr double kInt64MinAsDouble = -0x1p63;
  constexpr double kInt64UpperBoundAsDouble = 0x1p63;
  const std::array cases{
      ComparisonCase{std::numeric_limits<int64_t>::min(), kInt64MinAsDouble, 0, "exact INT64_MIN"},
      ComparisonCase{std::numeric_limits<int64_t>::max(), kInt64UpperBoundAsDouble, -1, "2^63 above INT64_MAX"},
      ComparisonCase{std::numeric_limits<int64_t>::max(), std::nextafter(kInt64UpperBoundAsDouble, 0.0), 1,
                     "largest representable double below 2^63"},
      ComparisonCase{std::numeric_limits<int64_t>::min(),
                     std::nextafter(kInt64MinAsDouble, -std::numeric_limits<double>::infinity()), 1,
                     "double below INT64_MIN"},
      ComparisonCase{int64_t{9'007'199'254'740'993LL}, 0x1p53, 1, "2^53 + 1 remains above 2^53"},
      ComparisonCase{42, 42.5, -1, "positive fractional double"},
      ComparisonCase{-42, -42.5, 1, "negative fractional double"},
      ComparisonCase{0, std::numeric_limits<double>::infinity(), -1, "positive infinity"},
      ComparisonCase{0, -std::numeric_limits<double>::infinity(), 1, "negative infinity"},
  };
  constexpr std::array operators{FilterOp::kEq, FilterOp::kNe, FilterOp::kGt,
                                 FilterOp::kGe, FilterOp::kLt, FilterOp::kLe};

  const auto matches = [](FilterOp op, int comparison) {
    switch (op) {
      case FilterOp::kEq:
        return comparison == 0;
      case FilterOp::kNe:
        return comparison != 0;
      case FilterOp::kGt:
        return comparison > 0;
      case FilterOp::kGe:
        return comparison >= 0;
      case FilterOp::kLt:
        return comparison < 0;
      case FilterOp::kLe:
        return comparison <= 0;
      case FilterOp::kIn:
        return false;
    }
    return false;
  };

  for (const auto& test_case : cases) {
    for (const FilterOp op : operators) {
      FilterCondition int_to_double;
      int_to_double.op = op;
      int_to_double.value = test_case.real;
      EXPECT_EQ(int_to_double.Match(MetadataValue(test_case.integer)), matches(op, test_case.comparison))
          << test_case.description << " int64 -> double, op=" << static_cast<int>(op);

      FilterCondition double_to_int;
      double_to_int.op = op;
      double_to_int.value = test_case.integer;
      EXPECT_EQ(double_to_int.Match(MetadataValue(test_case.real)), matches(op, -test_case.comparison))
          << test_case.description << " double -> int64, op=" << static_cast<int>(op);
    }
  }
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
  cond.values = {MetadataValue(std::string("news")), MetadataValue(std::string("blog"))};

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
