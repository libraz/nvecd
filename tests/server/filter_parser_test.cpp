/**
 * @file filter_parser_test.cpp
 * @brief Tests for filter expression parser
 */

#include "server/filter_parser.h"

#include <gtest/gtest.h>

namespace nvecd::server {
namespace {

TEST(FilterParserTest, EmptyExpression) {
  auto result = ParseSimpleFilter("");
  ASSERT_TRUE(result.has_value());
  EXPECT_TRUE(result->conditions.empty());
}

TEST(FilterParserTest, SingleStringCondition) {
  auto result = ParseSimpleFilter("status:active");
  ASSERT_TRUE(result.has_value());
  ASSERT_EQ(result->conditions.size(), 1U);
  EXPECT_EQ(result->conditions[0].field, "status");
  EXPECT_EQ(result->conditions[0].op, vectors::FilterOp::kEq);
  EXPECT_EQ(std::get<std::string>(result->conditions[0].value), "active");
}

TEST(FilterParserTest, MultipleConditions) {
  auto result = ParseSimpleFilter("status:active,category:news");
  ASSERT_TRUE(result.has_value());
  ASSERT_EQ(result->conditions.size(), 2U);
  EXPECT_EQ(result->conditions[0].field, "status");
  EXPECT_EQ(std::get<std::string>(result->conditions[0].value), "active");
  EXPECT_EQ(result->conditions[1].field, "category");
  EXPECT_EQ(std::get<std::string>(result->conditions[1].value), "news");
}

TEST(FilterParserTest, IntegerValue) {
  auto result = ParseSimpleFilter("count:42");
  ASSERT_TRUE(result.has_value());
  ASSERT_EQ(result->conditions.size(), 1U);
  EXPECT_EQ(std::get<int64_t>(result->conditions[0].value), 42);
}

TEST(FilterParserTest, NegativeIntegerValue) {
  auto result = ParseSimpleFilter("offset:-10");
  ASSERT_TRUE(result.has_value());
  ASSERT_EQ(result->conditions.size(), 1U);
  EXPECT_EQ(std::get<int64_t>(result->conditions[0].value), -10);
}

TEST(FilterParserTest, DoubleValue) {
  auto result = ParseSimpleFilter("score:0.95");
  ASSERT_TRUE(result.has_value());
  ASSERT_EQ(result->conditions.size(), 1U);
  EXPECT_DOUBLE_EQ(std::get<double>(result->conditions[0].value), 0.95);
}

TEST(FilterParserTest, BooleanTrue) {
  auto result = ParseSimpleFilter("active:true");
  ASSERT_TRUE(result.has_value());
  ASSERT_EQ(result->conditions.size(), 1U);
  EXPECT_EQ(std::get<bool>(result->conditions[0].value), true);
}

TEST(FilterParserTest, BooleanFalse) {
  auto result = ParseSimpleFilter("deleted:false");
  ASSERT_TRUE(result.has_value());
  ASSERT_EQ(result->conditions.size(), 1U);
  EXPECT_EQ(std::get<bool>(result->conditions[0].value), false);
}

TEST(FilterParserTest, MixedTypes) {
  auto result = ParseSimpleFilter("status:active,count:5,score:0.9,flag:true");
  ASSERT_TRUE(result.has_value());
  ASSERT_EQ(result->conditions.size(), 4U);
  EXPECT_EQ(std::get<std::string>(result->conditions[0].value), "active");
  EXPECT_EQ(std::get<int64_t>(result->conditions[1].value), 5);
  EXPECT_DOUBLE_EQ(std::get<double>(result->conditions[2].value), 0.9);
  EXPECT_EQ(std::get<bool>(result->conditions[3].value), true);
}

TEST(FilterParserTest, MissingColon) {
  auto result = ParseSimpleFilter("invalidformat");
  EXPECT_FALSE(result.has_value());
}

TEST(FilterParserTest, EmptyKey) {
  auto result = ParseSimpleFilter(":value");
  EXPECT_FALSE(result.has_value());
}

TEST(FilterParserTest, EmptyValue) {
  auto result = ParseSimpleFilter("key:");
  EXPECT_FALSE(result.has_value());
}

TEST(FilterParserTest, ValueWithColon) {
  // "url:http://example.com" — first colon splits key/value
  auto result = ParseSimpleFilter("url:http://example.com");
  ASSERT_TRUE(result.has_value());
  ASSERT_EQ(result->conditions.size(), 1U);
  EXPECT_EQ(result->conditions[0].field, "url");
  EXPECT_EQ(std::get<std::string>(result->conditions[0].value), "http://example.com");
}

TEST(FilterParserTest, TrailingComma) {
  auto result = ParseSimpleFilter("status:active,");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->conditions.size(), 1U);
}

TEST(FilterParserTest, ComparisonAndInOperators) {
  auto result = ParseSimpleFilter("rating>=4.5,count<10,status!=archived,category=in(books|music)");
  ASSERT_TRUE(result.has_value());
  ASSERT_EQ(result->conditions.size(), 4U);
  EXPECT_EQ(result->conditions[0].op, vectors::FilterOp::kGe);
  EXPECT_DOUBLE_EQ(std::get<double>(result->conditions[0].value), 4.5);
  EXPECT_EQ(result->conditions[1].op, vectors::FilterOp::kLt);
  EXPECT_EQ(result->conditions[2].op, vectors::FilterOp::kNe);
  EXPECT_EQ(result->conditions[3].op, vectors::FilterOp::kIn);
  ASSERT_EQ(result->conditions[3].values.size(), 2U);
  EXPECT_EQ(std::get<std::string>(result->conditions[3].values[1]), "music");
}

TEST(FilterParserTest, OnlyFiniteDecimalLiteralsBecomeNumbers) {
  auto result = ParseSimpleFilter("a:nan,b:inf,c:-infinity,d:0x10,e:1e400,f:2.5e-3,g:99999999999999999999");
  ASSERT_TRUE(result.has_value());
  ASSERT_EQ(result->conditions.size(), 7U);
  EXPECT_EQ(std::get<std::string>(result->conditions[0].value), "nan");
  EXPECT_EQ(std::get<std::string>(result->conditions[1].value), "inf");
  EXPECT_EQ(std::get<std::string>(result->conditions[2].value), "-infinity");
  EXPECT_EQ(std::get<std::string>(result->conditions[3].value), "0x10");
  EXPECT_EQ(std::get<std::string>(result->conditions[4].value), "1e400");
  EXPECT_DOUBLE_EQ(std::get<double>(result->conditions[5].value), 2.5e-3);
  // Beyond int64 an integer spelling is still a finite decimal: a double.
  EXPECT_DOUBLE_EQ(std::get<double>(result->conditions[6].value), 1e20);
}

TEST(FilterParserTest, MetadataPairsStoreExactlyTheWrittenValues) {
  auto metadata = ParseMetadataPairs("category:books,price=12,active:true");
  ASSERT_TRUE(metadata.has_value()) << metadata.error().message();
  ASSERT_EQ(metadata->size(), 3U);
  EXPECT_EQ(std::get<std::string>(metadata->at("category")), "books");
  EXPECT_EQ(std::get<int64_t>(metadata->at("price")), 12);
  EXPECT_TRUE(std::get<bool>(metadata->at("active")));
}

TEST(FilterParserTest, MetadataPairsRejectComparisonsInListsAndEmptyLists) {
  for (const char* expr : {"price>10", "price>=10", "price<10", "k!=v", "k=in(a|b)", "", ",", "a:1,b<2"}) {
    auto metadata = ParseMetadataPairs(expr);
    EXPECT_FALSE(metadata.has_value()) << expr;
  }
}

}  // namespace
}  // namespace nvecd::server
