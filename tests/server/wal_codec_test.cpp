/**
 * @file wal_codec_test.cpp
 * @brief Round-trip and bounds tests for the WAL command codec
 */

#include "server/wal_codec.h"

#include <gtest/gtest.h>

#include <cstdint>

#include "server/command_parser.h"
#include "storage/wal.h"
#include "storage/wal_checkpoint.h"

namespace nvecd::server {
namespace {

using events::EventType;
using storage::WalOpType;
using storage::WalRecord;

/// Build a WalRecord from an encoded command, mirroring what Append/Replay carry.
WalRecord MakeRecord(const Command& cmd) {
  WalRecord record;
  record.op = WalOpForCommand(cmd);
  auto payload = EncodeCommand(cmd);
  record.payload.assign(payload.begin(), payload.end());
  return record;
}

TEST(WalCodecTest, EventAddRoundTrip) {
  Command cmd;
  cmd.type = CommandType::kEvent;
  cmd.event_type = EventType::ADD;
  cmd.ctx = "session-42";
  cmd.id = "item-7";
  cmd.score = 5;
  cmd.timestamp = 1719400000ULL;

  EXPECT_EQ(WalOpForCommand(cmd), WalOpType::kEventAdd);

  auto decoded = DecodeWalRecord(MakeRecord(cmd));
  ASSERT_TRUE(decoded.has_value()) << decoded.error().to_string();
  EXPECT_EQ(decoded->type, CommandType::kEvent);
  EXPECT_EQ(decoded->event_type, EventType::ADD);
  EXPECT_EQ(decoded->ctx, "session-42");
  EXPECT_EQ(decoded->id, "item-7");
  EXPECT_EQ(decoded->score, 5);
  ASSERT_TRUE(decoded->timestamp.has_value());
  EXPECT_EQ(decoded->timestamp.value(), 1719400000ULL);
}

TEST(WalCodecTest, EventDelMapsToDeleteOpAndNegativeScore) {
  Command cmd;
  cmd.type = CommandType::kEvent;
  cmd.event_type = EventType::DEL;
  cmd.ctx = "ctx";
  cmd.id = "id";
  cmd.score = -3;
  // No timestamp set: should decode to 0.

  EXPECT_EQ(WalOpForCommand(cmd), WalOpType::kEventDel);

  auto decoded = DecodeWalRecord(MakeRecord(cmd));
  ASSERT_TRUE(decoded.has_value()) << decoded.error().to_string();
  EXPECT_EQ(decoded->event_type, EventType::DEL);
  EXPECT_EQ(decoded->score, -3);
  ASSERT_TRUE(decoded->timestamp.has_value());
  EXPECT_EQ(decoded->timestamp.value(), 0ULL);
}

TEST(WalCodecTest, VecsetMultiDimRoundTrip) {
  Command cmd;
  cmd.type = CommandType::kVecset;
  cmd.id = "vec-1";
  cmd.vector = {0.0F, -1.5F, 3.14159F, 1e9F, -2.71828F};

  EXPECT_EQ(WalOpForCommand(cmd), WalOpType::kVecSet);

  auto decoded = DecodeWalRecord(MakeRecord(cmd));
  ASSERT_TRUE(decoded.has_value()) << decoded.error().to_string();
  EXPECT_EQ(decoded->type, CommandType::kVecset);
  EXPECT_EQ(decoded->id, "vec-1");
  ASSERT_EQ(decoded->vector.size(), cmd.vector.size());
  for (size_t i = 0; i < cmd.vector.size(); ++i) {
    EXPECT_FLOAT_EQ(decoded->vector[i], cmd.vector[i]) << "element " << i;
  }
  EXPECT_EQ(decoded->dimension, static_cast<int>(cmd.vector.size()));
}

TEST(WalCodecTest, VecsetEmptyVectorRoundTrip) {
  Command cmd;
  cmd.type = CommandType::kVecset;
  cmd.id = "empty";

  auto decoded = DecodeWalRecord(MakeRecord(cmd));
  ASSERT_TRUE(decoded.has_value()) << decoded.error().to_string();
  EXPECT_EQ(decoded->id, "empty");
  EXPECT_TRUE(decoded->vector.empty());
}

TEST(WalCodecTest, MetasetRoundTrip) {
  Command cmd;
  cmd.type = CommandType::kMetaset;
  cmd.id = "doc-9";
  cmd.filter_expr = "status:active,type:news";

  EXPECT_EQ(WalOpForCommand(cmd), WalOpType::kMetaSet);

  auto decoded = DecodeWalRecord(MakeRecord(cmd));
  ASSERT_TRUE(decoded.has_value()) << decoded.error().to_string();
  EXPECT_EQ(decoded->type, CommandType::kMetaset);
  EXPECT_EQ(decoded->id, "doc-9");
  EXPECT_EQ(decoded->filter_expr, "status:active,type:news");
}

TEST(WalCodecTest, MetasetEmptyFilterRoundTrip) {
  Command cmd;
  cmd.type = CommandType::kMetaset;
  cmd.id = "doc-empty";
  cmd.filter_expr = "";

  auto decoded = DecodeWalRecord(MakeRecord(cmd));
  ASSERT_TRUE(decoded.has_value()) << decoded.error().to_string();
  EXPECT_EQ(decoded->id, "doc-empty");
  EXPECT_TRUE(decoded->filter_expr.empty());
}

TEST(WalCodecTest, EncodeUnsupportedCommandReturnsEmpty) {
  Command cmd;
  cmd.type = CommandType::kSim;
  cmd.id = "x";
  EXPECT_TRUE(EncodeCommand(cmd).empty());
}

TEST(WalCodecTest, TruncatedEventPayloadReturnsError) {
  Command cmd;
  cmd.type = CommandType::kEvent;
  cmd.event_type = EventType::ADD;
  cmd.ctx = "ctx";
  cmd.id = "id";
  cmd.score = 1;
  cmd.timestamp = 123ULL;

  WalRecord record = MakeRecord(cmd);
  ASSERT_FALSE(record.payload.empty());
  // Drop the trailing bytes so the timestamp field is incomplete.
  record.payload.resize(record.payload.size() - 4);

  auto decoded = DecodeWalRecord(record);
  EXPECT_FALSE(decoded.has_value());
}

TEST(WalCodecTest, TruncatedVecsetPayloadReturnsError) {
  Command cmd;
  cmd.type = CommandType::kVecset;
  cmd.id = "vec";
  cmd.vector = {1.0F, 2.0F, 3.0F};

  WalRecord record = MakeRecord(cmd);
  // Claim 3 dims but cut off mid-vector.
  record.payload.resize(record.payload.size() - 6);

  auto decoded = DecodeWalRecord(record);
  EXPECT_FALSE(decoded.has_value());
}

TEST(WalCodecTest, EmptyPayloadStringLengthReturnsError) {
  WalRecord record;
  record.op = WalOpType::kMetaSet;
  // No bytes at all: cannot even read the id length prefix.
  auto decoded = DecodeWalRecord(record);
  EXPECT_FALSE(decoded.has_value());
}

TEST(WalCheckpointTest, SidecarRoundTrip) {
  const std::string snapshot_path = ::testing::TempDir() + "/wal_codec_test_snapshot.bin";
  const uint64_t sequence = 0xA1B2C3D4E5F6ULL;

  auto write_result = storage::WriteWalCheckpoint(snapshot_path, sequence);
  ASSERT_TRUE(write_result.has_value()) << write_result.error().to_string();

  EXPECT_EQ(storage::ReadWalCheckpoint(snapshot_path), sequence);

  // Absent sidecar returns 0.
  EXPECT_EQ(storage::ReadWalCheckpoint(::testing::TempDir() + "/wal_codec_test_missing.bin"), 0ULL);

  ::unlink((snapshot_path + storage::kWalCheckpointSuffix).c_str());
}

}  // namespace
}  // namespace nvecd::server
