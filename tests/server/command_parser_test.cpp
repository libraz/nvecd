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
  auto result = ParseCommand("VECSET item123 0.1 0.2 0.3 0.4");
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
  // Dimension is now auto-detected, so this test is not applicable
  // We test with minimum floats requirement instead
  auto result = ParseCommand("VECSET item123");  // Missing floats
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), ErrorCode::kCommandSyntaxError);
}

TEST(CommandParserTest, ParseVecset_MissingVector) {
  auto result = ParseCommand("VECSET item123");  // No floats
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), ErrorCode::kCommandSyntaxError);
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
  auto result = ParseCommand("SIMV 5 0.5 0.6 0.7");
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
  // Dimension is now auto-detected, test with missing floats instead
  auto result = ParseCommand("SIMV 5");  // Missing floats
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), ErrorCode::kCommandSyntaxError);
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

// CACHE command tests
TEST(CommandParserTest, ParseCacheStats) {
  auto result = ParseCommand("CACHE STATS");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->type, CommandType::kCacheStats);
}

TEST(CommandParserTest, ParseCacheClear) {
  auto result = ParseCommand("CACHE CLEAR");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->type, CommandType::kCacheClear);
}

TEST(CommandParserTest, ParseCacheEnable) {
  auto result = ParseCommand("CACHE ENABLE");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->type, CommandType::kCacheEnable);
}

TEST(CommandParserTest, ParseCacheDisable) {
  auto result = ParseCommand("CACHE DISABLE");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->type, CommandType::kCacheDisable);
}

TEST(CommandParserTest, ParseCache_MissingSubcommand) {
  auto result = ParseCommand("CACHE");
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), ErrorCode::kCommandSyntaxError);
}

TEST(CommandParserTest, ParseCache_InvalidSubcommand) {
  auto result = ParseCommand("CACHE INVALID");
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), ErrorCode::kCommandSyntaxError);
}

TEST(CommandParserTest, ParseCache_CaseInsensitive) {
  auto result = ParseCommand("cache stats");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->type, CommandType::kCacheStats);
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
