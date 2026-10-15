/**
 * @file argument_validation_test.cpp
 * @brief Tests for the field validators shared by the TCP parser and HTTP handlers
 */

#include "server/argument_validation.h"

#include <gtest/gtest.h>

namespace nvecd::server {
namespace {

TEST(ArgumentValidationTest, LeadingPlusIsAcceptedOnce) {
  EXPECT_EQ(ParseDecimalInteger("+5"), 5);
  EXPECT_EQ(ParseDecimalInteger("-5"), -5);
  auto token = ParseIntegerToken("+5");
  ASSERT_TRUE(token.has_value());
  EXPECT_EQ(*token, 5);
}

TEST(ArgumentValidationTest, SecondSignAfterPlusIsRejected) {
  for (const char* text : {"+-5", "++5", "+-0"}) {
    EXPECT_FALSE(ParseDecimalInteger(text).has_value()) << text;
    EXPECT_FALSE(ParseIntegerToken(text).has_value()) << text;
    EXPECT_FALSE(ParseTopK(text, 0).has_value()) << text;
  }
}

TEST(ArgumentValidationTest, EventTypeIsCaseInsensitive) {
  EXPECT_EQ(ParseEventType("ADD"), events::EventType::ADD);
  EXPECT_EQ(ParseEventType("add"), events::EventType::ADD);
  EXPECT_EQ(ParseEventType("Set"), events::EventType::SET);
  EXPECT_EQ(ParseEventType("dEl"), events::EventType::DEL);
  EXPECT_FALSE(ParseEventType("").has_value());
  EXPECT_FALSE(ParseEventType("ADDS").has_value());
  EXPECT_FALSE(ParseEventType("REMOVE").has_value());
}

TEST(ArgumentValidationTest, SearchModesAreExactlyTheNamedSet) {
  EXPECT_TRUE(IsValidSearchMode(kSearchModeEvents));
  EXPECT_TRUE(IsValidSearchMode(kSearchModeVectors));
  EXPECT_TRUE(IsValidSearchMode(kSearchModeFusion));
  EXPECT_FALSE(IsValidSearchMode("Fusion"));
  EXPECT_FALSE(IsValidSearchMode(""));
  EXPECT_FALSE(IsValidSearchMode("vector"));
}

}  // namespace
}  // namespace nvecd::server
