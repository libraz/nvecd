/**
 * @file protocol_framing_test.cpp
 * @brief Wire framing of handler output, checked against the client's reply scanner
 */

#include "server/protocol_framing.h"

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <string_view>

#include "client/protocol_transport.h"

namespace nvecd::server {
namespace {

namespace transport = nvecd::client::transport;

/// Length of the first complete reply in @p wire as the client frames @p command.
std::optional<size_t> ClientReplyLength(const std::string& wire, std::string_view command) {
  return transport::CompleteResponseLength(wire, transport::FrameForCommand(command, false));
}

TEST(ProtocolFramingTest, EmptyBulkValueKeepsItsPayloadLine) {
  const std::string framed = NormalizeResponse("$0\r\n\r\n");
  EXPECT_EQ(framed, "$0\r\n\r\n");

  // A reply pipelined behind it must not be absorbed into the bulk value.
  const std::string wire = framed + NormalizeResponse("+OK\r\n");
  const auto first = ClientReplyLength(wire, "GET logging.file");
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ(*first, framed.size());
  EXPECT_EQ(wire.substr(*first), "+OK\r\n");
}

TEST(ProtocolFramingTest, BulkPayloadIsCopiedVerbatimSoItsLengthHolds) {
  EXPECT_EQ(NormalizeResponse("$3\r\na\nb\r\n"), "$3\r\na\nb\r\n");
  EXPECT_EQ(NormalizeResponse("*2\r\n$3\r\nx=1\r\n$0\r\n\r\n"), "*2\r\n$3\r\nx=1\r\n$0\r\n\r\n");
}

TEST(ProtocolFramingTest, MultiLineErrorBecomesOneLine) {
  const std::string framed = NormalizeResponse("ERROR first\n\nsecond\r\nthird\n");
  EXPECT_EQ(framed, "ERROR first second third\r\n");

  const std::string next = NormalizeResponse("OK INFO\r\nversion: x\r\nEND\r\n");
  const std::string wire = framed + next;
  const auto first = ClientReplyLength(wire, "CONFIG VERIFY bad.yaml");
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ(*first, framed.size());
  EXPECT_EQ(ClientReplyLength(wire.substr(*first), "INFO").value_or(0), next.size());
}

TEST(ProtocolFramingTest, LineEndingsBecomeCrlfWithoutDroppingLines) {
  EXPECT_EQ(NormalizeResponse("OK"), "OK\r\n");
  EXPECT_EQ(NormalizeResponse("OK\n"), "OK\r\n");
  EXPECT_EQ(NormalizeResponse("+OK\nkey: v\n\nEND\r\n"), "+OK\r\nkey: v\r\n\r\nEND\r\n");
  EXPECT_EQ(NormalizeResponse(""), "\r\n");
}

}  // namespace
}  // namespace nvecd::server
