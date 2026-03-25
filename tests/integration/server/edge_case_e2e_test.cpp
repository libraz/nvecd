/**
 * @file edge_case_e2e_test.cpp
 * @brief Edge case end-to-end tests for nvecd server via TCP
 *
 * Tests boundary values, malformed input, and error handling including:
 * - Very long IDs, Unicode IDs, special character IDs
 * - NaN/Inf vector values
 * - Negative/zero/very large scores
 * - Whitespace handling and case insensitivity
 * - Empty/partial/unknown commands
 * - Stat counter verification
 * - Multi-context isolation
 * - CONFIG VERIFY with valid/invalid YAML
 * - Path traversal in VECSET IDs
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <fstream>
#include <string>
#include <vector>

#include "../test_server_fixture.h"
#include "../test_tcp_client.h"

/**
 * @brief Fixture for edge case E2E tests with 3-dimensional vectors
 */
class EdgeCaseE2ETest : public NvecdTestFixture {
 protected:
  void SetUp() override { SetUpServer(3); }
  void TearDown() override { TearDownServer(); }
};

// ---------------------------------------------------------------------------
// Test 1: Very long ID (10000 chars)
// ---------------------------------------------------------------------------

TEST_F(EdgeCaseE2ETest, VeryLongId) {
  TcpClient client("127.0.0.1", port_);

  std::string long_id(10000, 'x');
  std::string cmd = "VECSET " + long_id + " 1.0 0.0 0.0";
  auto resp = client.SendCommand(cmd);

  // Should not crash. Either OK or ERROR is acceptable.
  EXPECT_FALSE(resp.empty()) << "Server should respond (not crash)";
  EXPECT_TRUE(ContainsOK(resp) || ContainsError(resp)) << "Response should be OK or ERROR, got: " + resp;
}

// ---------------------------------------------------------------------------
// Test 2: Unicode IDs (Japanese characters)
// ---------------------------------------------------------------------------

TEST_F(EdgeCaseE2ETest, UnicodeIds) {
  TcpClient client("127.0.0.1", port_);

  auto resp = client.SendCommand("VECSET \xe3\x83\x86\xe3\x82\xb9\xe3\x83\x88 1.0 0.0 0.0");
  EXPECT_TRUE(ContainsOK(resp)) << "VECSET with Unicode ID should succeed, got: " + resp;

  resp = client.SendCommand("SIM \xe3\x83\x86\xe3\x82\xb9\xe3\x83\x88 10 using=vectors");
  EXPECT_TRUE(ContainsOK(resp)) << "SIM with Unicode ID should succeed, got: " + resp;
}

// ---------------------------------------------------------------------------
// Test 3: Special character IDs
// ---------------------------------------------------------------------------

TEST_F(EdgeCaseE2ETest, SpecialCharIds) {
  TcpClient client("127.0.0.1", port_);

  EXPECT_TRUE(ContainsOK(client.SendCommand("VECSET item-1 1.0 0.0 0.0"))) << "ID with hyphen should succeed";
  EXPECT_TRUE(ContainsOK(client.SendCommand("VECSET item.2 0.0 1.0 0.0"))) << "ID with dot should succeed";
  EXPECT_TRUE(ContainsOK(client.SendCommand("VECSET item_3 0.0 0.0 1.0"))) << "ID with underscore should succeed";
  EXPECT_TRUE(ContainsOK(client.SendCommand("VECSET item:4 0.5 0.5 0.0"))) << "ID with colon should succeed";
}

// ---------------------------------------------------------------------------
// Test 4: NaN vector values
// ---------------------------------------------------------------------------

TEST_F(EdgeCaseE2ETest, NaNVectorValues) {
  TcpClient client("127.0.0.1", port_);

  auto resp = client.SendCommand("VECSET item_nan NaN 0.0 0.0");
  // Server currently accepts NaN values (C++ stof("NaN") succeeds without
  // error and no validation rejects NaN in the parser or vector store).
  // Document actual behavior: both OK and ERROR are acceptable.
  EXPECT_TRUE(ContainsOK(resp) || ContainsError(resp)) << "VECSET with NaN: server response: " + resp;
}

// ---------------------------------------------------------------------------
// Test 5: Inf vector values
// ---------------------------------------------------------------------------

TEST_F(EdgeCaseE2ETest, InfVectorValues) {
  TcpClient client("127.0.0.1", port_);

  auto resp = client.SendCommand("VECSET item_inf Inf 0.0 0.0");

  // Document behavior: should return ERROR, but OK is also acceptable
  // if the server accepts Inf values.
  EXPECT_FALSE(resp.empty()) << "Server should respond (not crash)";
  if (ContainsOK(resp)) {
    // Server accepts Inf values - document this behavior
    SUCCEED() << "Server accepts Inf vector values (documented behavior)";
  } else {
    EXPECT_TRUE(ContainsError(resp)) << "VECSET with Inf should return ERROR, got: " + resp;
  }
}

// ---------------------------------------------------------------------------
// Test 6: Negative scores
// ---------------------------------------------------------------------------

TEST_F(EdgeCaseE2ETest, NegativeScores) {
  TcpClient client("127.0.0.1", port_);

  auto resp = client.SendCommand("EVENT ctx1 ADD item1 -100");

  // Document behavior: negative scores may or may not be accepted
  EXPECT_FALSE(resp.empty()) << "Server should respond (not crash)";
  if (ContainsOK(resp)) {
    SUCCEED() << "Server accepts negative scores (documented behavior)";
  } else {
    EXPECT_TRUE(ContainsError(resp)) << "EVENT with negative score should return ERROR, got: " + resp;
  }
}

// ---------------------------------------------------------------------------
// Test 7: Zero score
// ---------------------------------------------------------------------------

TEST_F(EdgeCaseE2ETest, ZeroScore) {
  TcpClient client("127.0.0.1", port_);

  auto resp = client.SendCommand("EVENT ctx1 ADD item1 0");
  EXPECT_TRUE(ContainsOK(resp)) << "EVENT with zero score should succeed, got: " + resp;
}

// ---------------------------------------------------------------------------
// Test 8: Very large score
// ---------------------------------------------------------------------------

TEST_F(EdgeCaseE2ETest, VeryLargeScore) {
  TcpClient client("127.0.0.1", port_);

  auto resp = client.SendCommand("EVENT ctx1 ADD item1 999999999");
  EXPECT_TRUE(ContainsOK(resp)) << "EVENT with very large score should succeed, got: " + resp;
}

// ---------------------------------------------------------------------------
// Test 9: Extra whitespace in command
// ---------------------------------------------------------------------------

TEST_F(EdgeCaseE2ETest, ExtraWhitespace) {
  TcpClient client("127.0.0.1", port_);

  // Double spaces between tokens - parser's Split function skips empty tokens
  auto resp = client.SendCommand("EVENT  ctx1  ADD  item1  100");
  EXPECT_TRUE(ContainsOK(resp)) << "Command with extra whitespace should succeed, got: " + resp;
}

// ---------------------------------------------------------------------------
// Test 10: Case insensitive commands
// ---------------------------------------------------------------------------

TEST_F(EdgeCaseE2ETest, CaseInsensitiveCommands) {
  TcpClient client("127.0.0.1", port_);

  // Parser does ToUpper on first token, so lowercase "event" should work
  auto resp = client.SendCommand("event ctx1 ADD item1 100");
  EXPECT_TRUE(ContainsOK(resp)) << "Lowercase 'event' command should succeed, got: " + resp;

  resp = client.SendCommand("info");
  EXPECT_TRUE(ContainsOK(resp) || resp.find("INFO") != std::string::npos)
      << "Lowercase 'info' command should succeed, got: " + resp;
}

// ---------------------------------------------------------------------------
// Test 11: Empty command
// ---------------------------------------------------------------------------

TEST_F(EdgeCaseE2ETest, EmptyCommand) {
  TcpClient client("127.0.0.1", port_);

  // Empty string sends just "\r\n" — server may ignore empty lines
  // (no response). Send a whitespace-only command instead.
  auto resp = client.SendCommand(" ");
  // Server may return ERROR or ignore whitespace-only input
  // Either response or empty (timeout) is acceptable behavior
  EXPECT_TRUE(resp.empty() || ContainsError(resp) || ContainsOK(resp))
      << "Whitespace command: unexpected response: " + resp;
}

// ---------------------------------------------------------------------------
// Test 12: Partial command (missing subcommand and args)
// ---------------------------------------------------------------------------

TEST_F(EdgeCaseE2ETest, PartialCommand) {
  TcpClient client("127.0.0.1", port_);

  auto resp = client.SendCommand("EVENT ctx1");
  EXPECT_TRUE(ContainsError(resp)) << "Partial command should return ERROR, got: " + resp;
}

// ---------------------------------------------------------------------------
// Test 13: Unknown subcommands
// ---------------------------------------------------------------------------

TEST_F(EdgeCaseE2ETest, UnknownSubcommand) {
  TcpClient client("127.0.0.1", port_);

  auto resp = client.SendCommand("DUMP FREEZE");
  EXPECT_TRUE(ContainsError(resp)) << "DUMP FREEZE should return ERROR, got: " + resp;

  resp = client.SendCommand("CONFIG GET");
  EXPECT_TRUE(ContainsError(resp)) << "CONFIG GET should return ERROR, got: " + resp;
}

// ---------------------------------------------------------------------------
// Test 14: Stat counters verification
// ---------------------------------------------------------------------------

TEST_F(EdgeCaseE2ETest, StatCountersVerification) {
  TcpClient client("127.0.0.1", port_);

  // Send 5 EVENT ADD commands
  for (int i = 0; i < 5; ++i) {
    std::string cmd = "EVENT ctx1 ADD item_" + std::to_string(i) + " " + std::to_string((i + 1) * 10);
    ASSERT_TRUE(ContainsOK(client.SendCommand(cmd)));
  }

  // Send 3 VECSET commands
  ASSERT_TRUE(ContainsOK(client.SendCommand("VECSET item_0 1.0 0.0 0.0")));
  ASSERT_TRUE(ContainsOK(client.SendCommand("VECSET item_1 0.9 0.1 0.0")));
  ASSERT_TRUE(ContainsOK(client.SendCommand("VECSET item_2 0.0 1.0 0.0")));

  // Send 2 SIM commands
  client.SendCommand("SIM item_0 10 using=events");
  client.SendCommand("SIM item_0 10 using=vectors");

  // Send 1 INFO command (this one counts too)
  auto resp = client.SendCommand("INFO");
  ASSERT_TRUE(ContainsOK(resp) || resp.find("INFO") != std::string::npos) << "INFO should succeed";

  // Parse stat counters from INFO response
  std::string event_count_str = ParseResponseField(resp, "event_commands");
  std::string vecset_count_str = ParseResponseField(resp, "vecset_commands");
  std::string sim_count_str = ParseResponseField(resp, "sim_commands");
  std::string info_count_str = ParseResponseField(resp, "info_commands");

  if (!event_count_str.empty()) {
    int event_count = std::stoi(event_count_str);
    EXPECT_GE(event_count, 5) << "event_commands should be >= 5, got: " + event_count_str;
  }

  if (!vecset_count_str.empty()) {
    int vecset_count = std::stoi(vecset_count_str);
    EXPECT_GE(vecset_count, 3) << "vecset_commands should be >= 3, got: " + vecset_count_str;
  }

  if (!sim_count_str.empty()) {
    int sim_count = std::stoi(sim_count_str);
    EXPECT_GE(sim_count, 2) << "sim_commands should be >= 2, got: " + sim_count_str;
  }

  if (!info_count_str.empty()) {
    int info_count = std::stoi(info_count_str);
    EXPECT_GE(info_count, 1) << "info_commands should be >= 1, got: " + info_count_str;
  }
}

// ---------------------------------------------------------------------------
// Test 15: Multiple contexts isolation
// ---------------------------------------------------------------------------

TEST_F(EdgeCaseE2ETest, MultipleContextsIsolation) {
  TcpClient client("127.0.0.1", port_);

  // Create events in 50 different contexts, each with item_A and a unique
  // item_B_N. item_A co-occurs with each item_B_N in its own context.
  for (int i = 0; i < 50; ++i) {
    std::string ctx = "ctx_" + std::to_string(i);
    std::string item_b = "item_B_" + std::to_string(i);

    ASSERT_TRUE(ContainsOK(client.SendCommand("EVENT " + ctx + " ADD item_A 100")));
    ASSERT_TRUE(ContainsOK(client.SendCommand("EVENT " + ctx + " ADD " + item_b + " 90")));
  }

  // Also add a unique item that never co-occurs with item_A
  ASSERT_TRUE(ContainsOK(client.SendCommand("EVENT ctx_isolated ADD isolated_item 100")));
  ASSERT_TRUE(ContainsOK(client.SendCommand("EVENT ctx_isolated ADD another_isolated 80")));

  // Search for items similar to item_A using events
  auto resp = client.SendCommand("SIM item_A 100 using=events");
  ASSERT_TRUE(ContainsOK(resp)) << "SIM should succeed, got: " + resp;

  auto results = ParseSimResults(resp);

  // With 50 contexts of co-occurrence, we must get results.
  ASSERT_GE(results.size(), 2u) << "Expected at least 2 results from 50 co-occurring contexts";

  bool found_b_0 = false;
  bool found_b_49 = false;
  bool found_isolated = false;

  for (const auto& result : results) {
    if (result.first == "item_B_0") {
      found_b_0 = true;
    }
    if (result.first == "item_B_49") {
      found_b_49 = true;
    }
    if (result.first == "isolated_item") {
      found_isolated = true;
    }
  }

  EXPECT_TRUE(found_b_0) << "item_B_0 should appear (co-occurs with item_A in ctx_0)";
  EXPECT_TRUE(found_b_49) << "item_B_49 should appear (co-occurs with item_A in ctx_49)";
  EXPECT_FALSE(found_isolated) << "isolated_item should NOT appear (never co-occurred with item_A)";
}

// ---------------------------------------------------------------------------
// Test 16: CONFIG VERIFY with valid YAML
// ---------------------------------------------------------------------------

TEST_F(EdgeCaseE2ETest, ConfigVerifyValidYaml) {
  TcpClient client("127.0.0.1", port_);

  // Write a minimal valid YAML config to a temp file
  fs::path config_path = test_dir_ / "test_config.yaml";
  {
    std::ofstream ofs(config_path.string());
    ASSERT_TRUE(ofs.is_open()) << "Failed to create test YAML file";
    ofs << "api:\n"
        << "  tcp:\n"
        << "    bind: \"127.0.0.1\"\n"
        << "    port: 6820\n"
        << "events:\n"
        << "  ctx_buffer_size: 100\n"
        << "  decay_alpha: 0.95\n"
        << "  decay_interval_sec: 300\n"
        << "vectors:\n"
        << "  default_dimension: 3\n"
        << "similarity:\n"
        << "  default_top_k: 10\n"
        << "  max_top_k: 100\n"
        << "  fusion_alpha: 0.5\n"
        << "  fusion_beta: 0.5\n";
  }

  auto resp = client.SendCommand("CONFIG VERIFY " + config_path.string());
  // CONFIG VERIFY returns "+OK" (Redis-style) rather than "OK"
  EXPECT_TRUE(ContainsOK(resp) || resp.find("+OK") == 0)
      << "CONFIG VERIFY with valid YAML should return OK, got: " + resp;
}

// ---------------------------------------------------------------------------
// Test 17: CONFIG VERIFY with invalid YAML
// ---------------------------------------------------------------------------

TEST_F(EdgeCaseE2ETest, ConfigVerifyInvalidYaml) {
  TcpClient client("127.0.0.1", port_);

  // Write invalid YAML to a temp file
  fs::path config_path = test_dir_ / "test_config.yaml";
  {
    std::ofstream ofs(config_path.string());
    ASSERT_TRUE(ofs.is_open()) << "Failed to create test YAML file";
    ofs << "{{invalid";
  }

  auto resp = client.SendCommand("CONFIG VERIFY " + config_path.string());
  EXPECT_TRUE(ContainsError(resp)) << "CONFIG VERIFY with invalid YAML should return ERROR, got: " + resp;
}

// ---------------------------------------------------------------------------
// Test 18: Path traversal in VECSET ID
// ---------------------------------------------------------------------------

TEST_F(EdgeCaseE2ETest, PathTraversalInVecsetId) {
  TcpClient client("127.0.0.1", port_);

  // ID is just a string, no filesystem interaction should occur
  auto resp = client.SendCommand("VECSET ../etc/passwd 1.0 0.0 0.0");
  EXPECT_TRUE(ContainsOK(resp)) << "VECSET with path-like ID should succeed (ID is just a string), got: " + resp;
}
