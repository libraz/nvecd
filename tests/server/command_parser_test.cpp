/**
 * @file command_parser_test.cpp
 * @brief Tests for command parser
 */

#include "server/command_parser.h"

#include <gtest/gtest.h>

using namespace nvecd::server;
using namespace nvecd::utils;

// EVENT command tests
TEST(CommandParserTest, ParseEvent_Valid) {
  auto result = ParseCommand("EVENT user123 item456 95");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->type, CommandType::kEvent);
  EXPECT_EQ(result->ctx, "user123");
  EXPECT_EQ(result->id, "item456");
  EXPECT_EQ(result->score, 95);
}

TEST(CommandParserTest, ParseEvent_MissingArgs) {
  auto result = ParseCommand("EVENT user123");
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), ErrorCode::kCommandSyntaxError);
}

TEST(CommandParserTest, ParseEvent_InvalidScore) {
  auto result = ParseCommand("EVENT user123 item456 abc");
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), ErrorCode::kCommandInvalidArgument);
}

// VECSET command tests
TEST(CommandParserTest, ParseVecset_Valid) {
  auto result = ParseCommand("VECSET item123 4 text\n0.1 0.2 0.3 0.4");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->type, CommandType::kVecset);
  EXPECT_EQ(result->id, "item123");
  EXPECT_EQ(result->dimension, 4);
  ASSERT_EQ(result->vector.size(), 4);
  EXPECT_FLOAT_EQ(result->vector[0], 0.1f);
  EXPECT_FLOAT_EQ(result->vector[1], 0.2f);
  EXPECT_FLOAT_EQ(result->vector[2], 0.3f);
  EXPECT_FLOAT_EQ(result->vector[3], 0.4f);
}

TEST(CommandParserTest, ParseVecset_DimensionMismatch) {
  auto result = ParseCommand("VECSET item123 4 text\n0.1 0.2");  // Only 2 values
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), ErrorCode::kCommandInvalidVector);
}

TEST(CommandParserTest, ParseVecset_MissingVector) {
  auto result = ParseCommand("VECSET item123 4 text");  // No second line
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), ErrorCode::kCommandInvalidVector);
}

// SIM command tests
TEST(CommandParserTest, ParseSim_Basic) {
  auto result = ParseCommand("SIM item123 10");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->type, CommandType::kSim);
  EXPECT_EQ(result->id, "item123");
  EXPECT_EQ(result->top_k, 10);
  EXPECT_EQ(result->mode, "fusion");  // Default mode
}

TEST(CommandParserTest, ParseSim_WithMode) {
  auto result = ParseCommand("SIM item123 20 using=events");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->type, CommandType::kSim);
  EXPECT_EQ(result->id, "item123");
  EXPECT_EQ(result->top_k, 20);
  EXPECT_EQ(result->mode, "events");
}

TEST(CommandParserTest, ParseSim_MissingArgs) {
  auto result = ParseCommand("SIM item123");
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), ErrorCode::kCommandSyntaxError);
}

// SIMV command tests
TEST(CommandParserTest, ParseSimv_Valid) {
  auto result = ParseCommand("SIMV 3 5\n0.5 0.6 0.7");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->type, CommandType::kSimv);
  EXPECT_EQ(result->dimension, 3);
  EXPECT_EQ(result->top_k, 5);
  ASSERT_EQ(result->vector.size(), 3);
  EXPECT_FLOAT_EQ(result->vector[0], 0.5f);
  EXPECT_FLOAT_EQ(result->vector[1], 0.6f);
  EXPECT_FLOAT_EQ(result->vector[2], 0.7f);
}

TEST(CommandParserTest, ParseSimv_DimensionMismatch) {
  auto result = ParseCommand("SIMV 3 5\n0.5 0.6");  // Only 2 values
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), ErrorCode::kCommandInvalidVector);
}

// INFO command tests
TEST(CommandParserTest, ParseInfo) {
  auto result = ParseCommand("INFO");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->type, CommandType::kInfo);
}

// CONFIG command tests
TEST(CommandParserTest, ParseConfigHelp) {
  auto result = ParseCommand("CONFIG HELP");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->type, CommandType::kConfigHelp);
}

TEST(CommandParserTest, ParseConfigShow_WithPath) {
  auto result = ParseCommand("CONFIG SHOW events.ctx_buffer_size");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->type, CommandType::kConfigShow);
  EXPECT_EQ(result->path, "events.ctx_buffer_size");
}

TEST(CommandParserTest, ParseConfigVerify) {
  auto result = ParseCommand("CONFIG VERIFY");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->type, CommandType::kConfigVerify);
}

// DUMP command tests
TEST(CommandParserTest, ParseDumpSave) {
  auto result = ParseCommand("DUMP SAVE /data/nvecd.dmp");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->type, CommandType::kDumpSave);
  EXPECT_EQ(result->path, "/data/nvecd.dmp");
}

TEST(CommandParserTest, ParseDumpLoad) {
  auto result = ParseCommand("DUMP LOAD");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->type, CommandType::kDumpLoad);
  EXPECT_TRUE(result->path.empty());
}

// DEBUG command tests
TEST(CommandParserTest, ParseDebugOn) {
  auto result = ParseCommand("DEBUG ON");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->type, CommandType::kDebugOn);
}

TEST(CommandParserTest, ParseDebugOff) {
  auto result = ParseCommand("DEBUG OFF");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->type, CommandType::kDebugOff);
}

TEST(CommandParserTest, ParseDebug_InvalidArg) {
  auto result = ParseCommand("DEBUG INVALID");
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), ErrorCode::kCommandSyntaxError);
}

// Unknown command tests
TEST(CommandParserTest, ParseUnknown) {
  auto result = ParseCommand("FOOBAR arg1 arg2");
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), ErrorCode::kCommandUnknown);
}

// Empty command tests
TEST(CommandParserTest, ParseEmpty) {
  auto result = ParseCommand("");
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), ErrorCode::kCommandSyntaxError);
}

// Case insensitivity tests
TEST(CommandParserTest, ParseCaseInsensitive) {
  auto result = ParseCommand("info");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->type, CommandType::kInfo);

  result = ParseCommand("Event user123 item456 10");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->type, CommandType::kEvent);
}
