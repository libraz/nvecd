/**
 * @file command_parser_test.cpp
 * @brief Tests for command parser
 */

#include "server/command_parser.h"

#include <gtest/gtest.h>

#include <string>
#include <variant>

using namespace nvecd::server;
using namespace nvecd::utils;

// EVENT command tests
TEST(CommandParserTest, ParseEvent_Valid) {
  auto result = ParseCommand("EVENT user123 ADD item456 95");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->type, CommandType::kEvent);
  EXPECT_EQ(result->ctx, "user123");
  EXPECT_EQ(result->id, "item456");
  EXPECT_EQ(result->score, 95);
  EXPECT_EQ(result->event_type, nvecd::events::EventType::ADD);
}

TEST(CommandParserTest, ParseEvent_MissingArgs) {
  auto result = ParseCommand("EVENT user123");
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), ErrorCode::kCommandSyntaxError);
}

TEST(CommandParserTest, RejectsEmbeddedNulInsteadOfSilentlyTruncating) {
  std::string request = "EVENT ctx ADD item 1";
  request.push_back('\0');
  request += " VECSET hidden 1 0";
  auto result = ParseCommand(request);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), ErrorCode::kCommandSyntaxError);
}

TEST(CommandParserTest, RejectsMultilineCommandInsteadOfIgnoringTrailingPayload) {
  auto result = ParseCommand("VECSET item 1 0\nVECSET hidden 1 1");
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), ErrorCode::kCommandSyntaxError);
}

TEST(CommandParserTest, ParseEvent_InvalidScore) {
  auto result = ParseCommand("EVENT user123 ADD item456 abc");
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), ErrorCode::kCommandInvalidArgument);
}

TEST(CommandParserTest, ParseEvent_NegativeScoreRejected) {
  auto result = ParseCommand("EVENT user123 ADD item456 -5");
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), ErrorCode::kEventInvalidScore);
}

TEST(CommandParserTest, ParseEvent_ScoreAboveMaxRejected) {
  auto result = ParseCommand("EVENT user123 SET item456 101");
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), ErrorCode::kEventInvalidScore);
}

TEST(CommandParserTest, ParseEvent_BoundaryScoresAccepted) {
  auto low = ParseCommand("EVENT user123 ADD item456 0");
  ASSERT_TRUE(low.has_value());
  EXPECT_EQ(low->score, 0);

  auto high = ParseCommand("EVENT user123 ADD item456 100");
  ASSERT_TRUE(high.has_value());
  EXPECT_EQ(high->score, 100);
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

TEST(CommandParserTest, ParseVecset_RejectsNonFiniteFloat) {
  auto result = ParseCommand("VECSET item123 1.0 nan");
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), ErrorCode::kCommandInvalidVector);

  result = ParseCommand("VECSET item123 1.0 inf");
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), ErrorCode::kCommandInvalidVector);
}

TEST(CommandParserTest, ParseVecdel_ValidAndRejectsMissingId) {
  auto result = ParseCommand("VECDEL item123");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->type, CommandType::kVecdel);
  EXPECT_EQ(result->id, "item123");

  result = ParseCommand("VECDEL");
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), ErrorCode::kCommandSyntaxError);
}

// METASET command tests
TEST(CommandParserTest, ParseMetaset_Valid) {
  auto result = ParseCommand("METASET item123 category:electronics,active:true");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->type, CommandType::kMetaset);
  EXPECT_EQ(result->id, "item123");
  ASSERT_TRUE(result->metadata.has_value());
  EXPECT_EQ(result->metadata->size(), 2u);
  EXPECT_EQ(std::get<std::string>(result->metadata->at("category")), "electronics");
  EXPECT_TRUE(std::get<bool>(result->metadata->at("active")));
}

TEST(CommandParserTest, ParseMetaset_RejectsAnythingButEqualityPairs) {
  for (const char* request : {"METASET x price>10", "METASET x price<=10", "METASET x k!=v", "METASET x k=in(a|b)",
                              "METASET x ,", "METASET x ,,,"}) {
    auto result = ParseCommand(request);
    ASSERT_FALSE(result.has_value()) << request;
    EXPECT_EQ(result.error().code(), ErrorCode::kCommandInvalidArgument) << request;
  }
}

TEST(CommandParserTest, ParseMetaset_MissingArgs) {
  auto result = ParseCommand("METASET item123");
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

TEST(CommandParserTest, ParseSim_TopKExceedsMaxRejected) {
  auto result = ParseCommand("SIM item123 200", /*max_top_k=*/100);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), ErrorCode::kCommandInvalidTopK);
}

TEST(CommandParserTest, ParseSim_TopKWithinMaxAccepted) {
  auto result = ParseCommand("SIM item123 100", /*max_top_k=*/100);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->top_k, 100);
}

TEST(CommandParserTest, ParseSim_TopKZeroRejected) {
  auto result = ParseCommand("SIM item123 0");
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), ErrorCode::kCommandInvalidTopK);
}

TEST(CommandParserTest, ParseSim_TopKNegativeRejected) {
  auto result = ParseCommand("SIM item123 -1");
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), ErrorCode::kCommandInvalidTopK);
}

TEST(CommandParserTest, ParseSimv_TopKExceedsMaxRejected) {
  auto result = ParseCommand("SIMV 200 0.1 0.2", /*max_top_k=*/100);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), ErrorCode::kCommandInvalidTopK);
}

// SIMV carries the same top_k bounds as SIM. Both ends are pinned on this
// command too, so the two commands cannot drift apart if either grows its own
// argument handling.
TEST(CommandParserTest, ParseSimv_TopKZeroRejected) {
  auto result = ParseCommand("SIMV 0 0.1 0.2");
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), ErrorCode::kCommandInvalidTopK);
}

TEST(CommandParserTest, ParseSimv_TopKNegativeRejected) {
  auto result = ParseCommand("SIMV -1 0.1 0.2");
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), ErrorCode::kCommandInvalidTopK);
}

TEST(CommandParserTest, ParseSimv_TopKWithinMaxAccepted) {
  auto result = ParseCommand("SIMV 100 0.1 0.2", /*max_top_k=*/100);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->top_k, 100);
}

TEST(CommandParserTest, ParseSim_MissingArgs) {
  auto result = ParseCommand("SIM item123");
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), ErrorCode::kCommandSyntaxError);
}

TEST(CommandParserTest, ParseSim_InvalidMode) {
  auto result = ParseCommand("SIM item123 10 using=unknown");
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), ErrorCode::kCommandSyntaxError);
}

TEST(CommandParserTest, ParseSim_UnsupportedCandidateLimit) {
  auto result = ParseCommand("SIM item123 10 candidate_limit=-1");
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

TEST(CommandParserTest, ParseSimv_MissingVectorAfterSupportedOptions) {
  auto result = ParseCommand("SIMV 5 min_score=0.1");
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), ErrorCode::kCommandSyntaxError);
}

TEST(CommandParserTest, ParseSimv_UnsupportedExplain) {
  auto result = ParseCommand("SIMV 5 explain=on");
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), ErrorCode::kCommandSyntaxError);
}

TEST(CommandParserTest, ParseSimv_UnsupportedCandidateLimit) {
  auto result = ParseCommand("SIMV 5 candidate_limit=100 0.1 0.2");
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

// ============================================================================
// EVENT timestamp parameter tests
// ============================================================================

TEST(CommandParserTest, EventAddWithTimestamp) {
  auto result = ParseCommand("EVENT ctx1 ADD item1 10 timestamp=1711411200");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->type, CommandType::kEvent);
  EXPECT_EQ(result->ctx, "ctx1");
  EXPECT_EQ(result->id, "item1");
  EXPECT_EQ(result->score, 10);
  ASSERT_TRUE(result->timestamp.has_value());
  EXPECT_EQ(*result->timestamp, 1711411200u);
}

TEST(CommandParserTest, EventSetWithTimestamp) {
  auto result = ParseCommand("EVENT ctx1 SET item1 10 timestamp=1234567890");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->type, CommandType::kEvent);
  ASSERT_TRUE(result->timestamp.has_value());
  EXPECT_EQ(*result->timestamp, 1234567890u);
}

TEST(CommandParserTest, EventDelWithTimestamp) {
  auto result = ParseCommand("EVENT ctx1 DEL item1 timestamp=9999");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->type, CommandType::kEvent);
  ASSERT_TRUE(result->timestamp.has_value());
  EXPECT_EQ(*result->timestamp, 9999u);
}

TEST(CommandParserTest, EventWithoutTimestamp) {
  auto result = ParseCommand("EVENT ctx1 ADD item1 10");
  ASSERT_TRUE(result.has_value());
  EXPECT_FALSE(result->timestamp.has_value());
}

TEST(CommandParserTest, EventInvalidTimestamp) {
  auto result = ParseCommand("EVENT ctx1 ADD item1 10 timestamp=abc");
  EXPECT_FALSE(result.has_value());
}

// ============================================================================
// SIM adaptive parameter tests
// ============================================================================

TEST(CommandParserTest, SimWithAdaptiveOn) {
  auto result = ParseCommand("SIM item1 10 adaptive=on");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->type, CommandType::kSim);
  ASSERT_TRUE(result->adaptive.has_value());
  EXPECT_TRUE(*result->adaptive);
}

TEST(CommandParserTest, SimWithAdaptiveOff) {
  auto result = ParseCommand("SIM item1 10 adaptive=off");
  ASSERT_TRUE(result.has_value());
  ASSERT_TRUE(result->adaptive.has_value());
  EXPECT_FALSE(*result->adaptive);
}

TEST(CommandParserTest, SimWithUsingAndAdaptive) {
  auto result = ParseCommand("SIM item1 10 using=fusion adaptive=on");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->mode, "fusion");
  ASSERT_TRUE(result->adaptive.has_value());
  EXPECT_TRUE(*result->adaptive);
}

TEST(CommandParserTest, SimWithAdaptiveInvalid) {
  auto result = ParseCommand("SIM item1 10 adaptive=maybe");
  EXPECT_FALSE(result.has_value());
}

// Case insensitivity tests
TEST(CommandParserTest, ParseCaseInsensitive) {
  auto result = ParseCommand("info");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->type, CommandType::kInfo);

  result = ParseCommand("Event user123 add item456 10");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->type, CommandType::kEvent);
  EXPECT_EQ(result->event_type, nvecd::events::EventType::ADD);
}

// ============================================================================
// Shared numeric validators
// ============================================================================

TEST(CommandParserTest, TimestampAcceptsOnlyUnsignedDigits) {
  for (const char* value : {"-1", "+5", "1e3", "\t5", "18446744073709551616", ""}) {
    auto result = ParseCommand(std::string("EVENT ctx1 ADD item1 10 timestamp=") + value);
    ASSERT_FALSE(result.has_value()) << value;
    EXPECT_EQ(result.error().code(), ErrorCode::kCommandInvalidArgument) << value;
  }
  auto del = ParseCommand("EVENT ctx1 DEL item1 timestamp=-1");
  ASSERT_FALSE(del.has_value());

  auto max = ParseCommand("EVENT ctx1 ADD item1 10 timestamp=18446744073709551615");
  ASSERT_TRUE(max.has_value()) << max.error().message();
  EXPECT_EQ(*max->timestamp, 18446744073709551615ULL);
}

TEST(CommandParserTest, TopKOverflowIsReportedAsTopKRangeError) {
  for (const char* request : {"SIM item1 4294967297", "SIM item1 99999999999999999999999", "SIMV 4294967297 0.1"}) {
    auto result = ParseCommand(request, 100);
    ASSERT_FALSE(result.has_value()) << request;
    EXPECT_EQ(result.error().code(), ErrorCode::kCommandInvalidTopK) << request;
  }
  auto unbounded = ParseCommand("SIM item1 2147483648", 0);
  ASSERT_FALSE(unbounded.has_value());
  EXPECT_EQ(unbounded.error().code(), ErrorCode::kCommandInvalidTopK);

  auto negative = ParseCommand("SIM item1 -99999999999999999999999", 100);
  ASSERT_FALSE(negative.has_value());
  EXPECT_EQ(negative.error().code(), ErrorCode::kCommandInvalidTopK);
}

TEST(CommandParserTest, DenormalAndUnderflowFloatsNarrowInsteadOfFailing) {
  auto vecset = ParseCommand("VECSET item1 1e-40 1e-50 0.5");
  ASSERT_TRUE(vecset.has_value()) << vecset.error().message();
  ASSERT_EQ(vecset->vector.size(), 3u);
  EXPECT_GT(vecset->vector[0], 0.0F);
  EXPECT_EQ(vecset->vector[1], 0.0F);

  auto simv = ParseCommand("SIMV 5 min_score=1e-45 1e-40 0.5");
  ASSERT_TRUE(simv.has_value()) << simv.error().message();
  EXPECT_EQ(simv->vector.size(), 2u);
}

TEST(CommandParserTest, FloatTokensRejectNonDecimalSpellings) {
  auto overflow = ParseCommand("VECSET item1 1e39 0.5");
  ASSERT_FALSE(overflow.has_value());
  EXPECT_EQ(overflow.error().message(), "Invalid float: 1e39");

  auto nan = ParseCommand("VECSET item1 nan 0.5");
  ASSERT_FALSE(nan.has_value());
  EXPECT_EQ(nan.error().message(), "Invalid float: nan");

  auto hex = ParseCommand("VECSET item1 0x10 0.5");
  ASSERT_FALSE(hex.has_value());
  EXPECT_EQ(hex.error().message(), "Invalid float: 0x10");

  auto word = ParseCommand("VECSET item1 abc 0.5");
  ASSERT_FALSE(word.has_value());
  EXPECT_EQ(word.error().message(), "Failed to parse float: abc");
}

TEST(CommandParserTest, FilterIsParsedWithTheCommand) {
  auto sim = ParseCommand("SIM item1 5 filter=price>10,status:active");
  ASSERT_TRUE(sim.has_value()) << sim.error().message();
  EXPECT_EQ(sim->filter_expr, "price>10,status:active");
  EXPECT_EQ(sim->filter.conditions.size(), 2u);

  auto bad = ParseCommand("SIMV 5 filter=novalue 0.1");
  ASSERT_FALSE(bad.has_value());
  EXPECT_EQ(bad.error().code(), ErrorCode::kCommandParseError);
}

// ============================================================================
// AUTH separator handling
// ============================================================================

TEST(CommandParserTest, AuthPasswordIsEverythingAfterTheSeparator) {
  struct Case {
    const char* request;
    const char* password;
  };
  for (const Case& c : {Case{"AUTH secret", "secret"}, Case{"AUTH\tsecret", "secret"}, Case{" AUTH s3cret", "s3cret"},
                        Case{"\tauth pass word ", "pass word "}, Case{"AUTH  lead", " lead"}}) {
    auto result = ParseCommand(c.request);
    ASSERT_TRUE(result.has_value()) << c.request;
    EXPECT_EQ(result->type, CommandType::kAuth) << c.request;
    EXPECT_EQ(result->variable_value, c.password) << c.request;
  }
  for (const char* request : {"AUTH", "AUTH ", "AUTH\t"}) {
    auto result = ParseCommand(request);
    ASSERT_FALSE(result.has_value()) << request;
    EXPECT_EQ(result.error().code(), ErrorCode::kCommandSyntaxError) << request;
  }
}
