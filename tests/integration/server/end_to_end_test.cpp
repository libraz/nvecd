/**
 * @file end_to_end_test.cpp
 * @brief End-to-end integration tests for nvecd server via TCP commands
 *
 * Tests the full server through TCP protocol commands including:
 * - EVENT (add/remove events)
 * - VECSET (set vectors)
 * - SIM (similarity search: events, vectors, fusion)
 * - SIMV (similarity search by vector)
 * - INFO, CONFIG, CACHE, DEBUG, SET/GET, SHOW VARIABLES
 * - Error handling for unknown/malformed commands
 * - Full pipeline workflow
 */

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <thread>

#include "../test_tcp_client.h"
#include "config/config.h"
#include "server/nvecd_server.h"

namespace fs = std::filesystem;

using namespace nvecd;
using namespace nvecd::server;

/**
 * @brief Test fixture for end-to-end integration tests
 *
 * Sets up a full nvecd server on a random port with small dimensions
 * for fast testing. Each test creates fresh TCP connections.
 */
class EndToEndTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // Create temporary test directory for snapshots
    test_dir_ = fs::temp_directory_path() / "nvecd_e2e_test";
    fs::create_directories(test_dir_);

    // Create configuration with small dimensions for fast tests
    config_.api.tcp.bind = "127.0.0.1";
    config_.api.tcp.port = 0;  // Random port
    config_.network.allow_cidrs = {"127.0.0.1/32"};
    config_.perf.max_connections = 10;
    config_.perf.thread_pool_size = 4;
    config_.snapshot.dir = test_dir_.string();

    config_.events.ctx_buffer_size = 100;
    config_.events.decay_alpha = 0.95;
    config_.events.decay_interval_sec = 300;

    config_.vectors.default_dimension = 3;

    config_.similarity.default_top_k = 10;
    config_.similarity.max_top_k = 100;
    config_.similarity.fusion_alpha = 0.5;
    config_.similarity.fusion_beta = 0.5;

    // Create and start server
    server_ = std::make_unique<NvecdServer>(config_);
    ASSERT_TRUE(server_->Start().has_value());

    // Get actual port and wait for server to be ready
    port_ = server_->GetPort();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  void TearDown() override {
    if (server_) {
      server_->Stop();
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // Clean up test directory
    if (fs::exists(test_dir_)) {
      fs::remove_all(test_dir_);
    }
  }

  fs::path test_dir_;
  config::Config config_;
  std::unique_ptr<NvecdServer> server_;
  uint16_t port_ = 0;
};

// ---------------------------------------------------------------------------
// EVENT tests
// ---------------------------------------------------------------------------

TEST_F(EndToEndTest, EventAdd_Basic) {
  TcpClient client("127.0.0.1", port_);
  EXPECT_EQ(client.SendCommand("EVENT ctx1 ADD item1 100"), "OK EVENT\r\n");
}

TEST_F(EndToEndTest, EventAdd_InvalidScore) {
  TcpClient client("127.0.0.1", port_);
  EXPECT_EQ(client.SendCommand("EVENT ctx1 ADD item1 notanumber"), "ERROR Failed to parse integer: notanumber\r\n");
}

TEST_F(EndToEndTest, EventAdd_MissingArgs) {
  TcpClient client("127.0.0.1", port_);
  EXPECT_EQ(client.SendCommand("EVENT ctx1 ADD"),
            "ERROR EVENT requires at least 3 arguments: <ctx> <type> <id> [<score>]\r\n");
}

// ---------------------------------------------------------------------------
// VECSET tests
// ---------------------------------------------------------------------------

TEST_F(EndToEndTest, Vecset_Basic) {
  TcpClient client("127.0.0.1", port_);
  EXPECT_EQ(client.SendCommand("VECSET item1 1.0 2.0 3.0"), "OK VECSET\r\n");
}

TEST_F(EndToEndTest, Vecset_DimensionMismatch) {
  TcpClient client("127.0.0.1", port_);
  // First set a vector with 3 dimensions
  ASSERT_EQ(client.SendCommand("VECSET item1 1.0 2.0 3.0"), "OK VECSET\r\n");
  // Try to set a different vector with 2 dimensions (mismatch)
  EXPECT_EQ(client.SendCommand("VECSET item2 1.0 2.0"), "ERROR Vector dimension mismatch: expected 3, got 2\r\n");
}

// ---------------------------------------------------------------------------
// SIM tests
// ---------------------------------------------------------------------------

TEST_F(EndToEndTest, Sim_EventBased) {
  TcpClient client("127.0.0.1", port_);
  // Setup: create co-occurrence data
  client.SendCommand("EVENT ctx1 ADD item1 100");
  client.SendCommand("EVENT ctx1 ADD item2 90");
  client.SendCommand("EVENT ctx1 ADD item3 80");
  // Search
  auto results = ParseSimResults(client.SendCommand("SIM item1 10 using=events"));
  // item1 pairs with item2 at 100*90 and with item3 at 100*80.
  ASSERT_EQ(results.size(), 2U);
  EXPECT_EQ(results[0].first, "item2");
  EXPECT_EQ(results[1].first, "item3");
}

TEST_F(EndToEndTest, Sim_VectorBased) {
  TcpClient client("127.0.0.1", port_);
  client.SendCommand("VECSET vec1 1.0 0.0 0.0");
  client.SendCommand("VECSET vec2 0.9 0.1 0.0");
  client.SendCommand("VECSET vec3 0.0 1.0 0.0");
  auto results = ParseSimResults(client.SendCommand("SIM vec1 10 using=vectors"));
  ASSERT_FALSE(results.empty());
  EXPECT_EQ(results[0].first, "vec2");
  for (const auto& [id, score] : results) {
    EXPECT_NE(id, "vec1");
  }
}

TEST_F(EndToEndTest, Sim_Fusion) {
  TcpClient client("127.0.0.1", port_);
  client.SendCommand("EVENT ctx1 ADD item1 100");
  client.SendCommand("EVENT ctx1 ADD item2 90");
  client.SendCommand("VECSET item1 1.0 0.0 0.0");
  client.SendCommand("VECSET item2 0.9 0.1 0.0");
  auto results = ParseSimResults(client.SendCommand("SIM item1 10 using=fusion"));
  ASSERT_EQ(results.size(), 1U);
  EXPECT_EQ(results[0].first, "item2");
}

TEST_F(EndToEndTest, Sim_NotFound_EventsIsEmpty) {
  TcpClient client("127.0.0.1", port_);
  EXPECT_EQ(client.SendCommand("SIM nonexistent 10 using=events"), "OK RESULTS 0\r\n");
}

TEST_F(EndToEndTest, Sim_NotFound_VectorsAndFusionAreErrors) {
  TcpClient client("127.0.0.1", port_);
  EXPECT_EQ(client.SendCommand("SIM nonexistent 10 using=vectors"), "ERROR Query vector not found: nonexistent\r\n");
  EXPECT_EQ(client.SendCommand("SIM nonexistent 10 using=fusion"), "ERROR Query vector not found: nonexistent\r\n");
}

// ---------------------------------------------------------------------------
// SIMV test
// ---------------------------------------------------------------------------

TEST_F(EndToEndTest, Simv_Basic) {
  TcpClient client("127.0.0.1", port_);
  client.SendCommand("VECSET vec1 1.0 0.0 0.0");
  client.SendCommand("VECSET vec2 0.9 0.1 0.0");
  client.SendCommand("VECSET vec3 0.0 1.0 0.0");
  auto results = ParseSimResults(client.SendCommand("SIMV 10 1.0 0.0 0.0"));
  ASSERT_FALSE(results.empty());
  EXPECT_EQ(results[0].first, "vec1");
  EXPECT_NEAR(results[0].second, 1.0F, 1e-3F);
}

// ---------------------------------------------------------------------------
// INFO tests
// ---------------------------------------------------------------------------

TEST_F(EndToEndTest, Info_EmptyServer) {
  TcpClient client("127.0.0.1", port_);
  auto resp = client.SendCommand("INFO");
  EXPECT_TRUE(resp.find("OK INFO") != std::string::npos);
  EXPECT_TRUE(resp.find("vector_count: 0") != std::string::npos);
  EXPECT_TRUE(resp.find("event_count: 0") != std::string::npos);
}

TEST_F(EndToEndTest, Info_AfterData) {
  TcpClient client("127.0.0.1", port_);
  client.SendCommand("EVENT ctx1 ADD item1 100");
  client.SendCommand("VECSET item1 1.0 2.0 3.0");
  auto resp = client.SendCommand("INFO");
  EXPECT_EQ(resp.find("OK INFO"), 0U);
  EXPECT_EQ(ParseResponseField(resp, "vector_count"), "1");
  EXPECT_EQ(ParseResponseField(resp, "event_count"), "1");
}

// ---------------------------------------------------------------------------
// CONFIG tests
// ---------------------------------------------------------------------------

TEST_F(EndToEndTest, ConfigHelp_Root) {
  TcpClient client("127.0.0.1", port_);
  auto resp = client.SendCommand("CONFIG HELP");
  EXPECT_EQ(resp.find("+OK\r\n"), 0U);
  EXPECT_TRUE(resp.size() >= 5 && resp.compare(resp.size() - 5, 5, "END\r\n") == 0);
  EXPECT_NE(resp.find("api"), std::string::npos);
}

TEST_F(EndToEndTest, ConfigHelp_Path) {
  TcpClient client("127.0.0.1", port_);
  auto resp = client.SendCommand("CONFIG HELP similarity.index_type");
  EXPECT_EQ(resp.find("+OK\r\nsimilarity.index_type\r\n"), 0U);
  EXPECT_TRUE(resp.size() >= 5 && resp.compare(resp.size() - 5, 5, "END\r\n") == 0);
  EXPECT_NE(resp.find("Type: string (enum)"), std::string::npos);
}

TEST_F(EndToEndTest, ConfigShow_Full) {
  TcpClient client("127.0.0.1", port_);
  auto resp = client.SendCommand("CONFIG SHOW");
  EXPECT_EQ(resp.find("+OK\r\n"), 0U);
  EXPECT_TRUE(resp.size() >= 5 && resp.compare(resp.size() - 5, 5, "END\r\n") == 0);
}

// ---------------------------------------------------------------------------
// CACHE tests
// ---------------------------------------------------------------------------

TEST_F(EndToEndTest, CacheStats_Empty) {
  TcpClient client("127.0.0.1", port_);
  auto resp = client.SendCommand("CACHE STATS");
  EXPECT_TRUE(resp.find("OK") == 0);
  EXPECT_TRUE(resp.find("cache_entries") != std::string::npos);
}

TEST_F(EndToEndTest, CacheClear) {
  TcpClient client("127.0.0.1", port_);
  EXPECT_EQ(client.SendCommand("CACHE CLEAR"), "OK CACHE_CLEARED\r\n");
}

TEST_F(EndToEndTest, CacheEnableDisable) {
  TcpClient client("127.0.0.1", port_);
  EXPECT_EQ(client.SendCommand("CACHE DISABLE"), "OK CACHE_DISABLED\r\n");
  auto resp = client.SendCommand("CACHE STATS");
  EXPECT_TRUE(resp.find("cache_enabled: false") != std::string::npos);
  EXPECT_EQ(client.SendCommand("CACHE ENABLE"), "OK CACHE_ENABLED\r\n");
  resp = client.SendCommand("CACHE STATS");
  EXPECT_TRUE(resp.find("cache_enabled: true") != std::string::npos);
}

// ---------------------------------------------------------------------------
// DEBUG tests
// ---------------------------------------------------------------------------

TEST_F(EndToEndTest, DebugOn_Off) {
  TcpClient client("127.0.0.1", port_);
  EXPECT_EQ(client.SendCommand("DEBUG ON"), "OK DEBUG_ON\r\n");
  EXPECT_EQ(client.SendCommand("DEBUG OFF"), "OK DEBUG_OFF\r\n");
}

// ---------------------------------------------------------------------------
// VARIABLES tests
// ---------------------------------------------------------------------------

TEST_F(EndToEndTest, SetGet_Variable) {
  TcpClient client("127.0.0.1", port_);
  EXPECT_EQ(client.SendCommand("SET cache.ttl_seconds 60"), "+OK\r\n");
  EXPECT_EQ(client.SendCommand("GET cache.ttl_seconds"), "$2\r\n60\r\n");
}

TEST_F(EndToEndTest, ShowVariables) {
  TcpClient client("127.0.0.1", port_);
  auto resp = client.SendCommand("SHOW VARIABLES");
  // Returns array format: *N\r\n...
  EXPECT_EQ(resp.rfind('*', 0), 0U);
}

// ---------------------------------------------------------------------------
// Full pipeline workflow test
// ---------------------------------------------------------------------------

TEST_F(EndToEndTest, FullPipeline) {
  TcpClient client("127.0.0.1", port_);

  // 1. Add events
  EXPECT_EQ(client.SendCommand("EVENT ctx1 ADD item1 100"), "OK EVENT\r\n");
  EXPECT_EQ(client.SendCommand("EVENT ctx1 ADD item2 90"), "OK EVENT\r\n");
  EXPECT_EQ(client.SendCommand("EVENT ctx1 ADD item3 80"), "OK EVENT\r\n");

  // 2. Set vectors
  EXPECT_EQ(client.SendCommand("VECSET item1 1.0 0.0 0.0"), "OK VECSET\r\n");
  EXPECT_EQ(client.SendCommand("VECSET item2 0.9 0.1 0.0"), "OK VECSET\r\n");
  EXPECT_EQ(client.SendCommand("VECSET item3 0.0 1.0 0.0"), "OK VECSET\r\n");

  // 3. SIM search (events)
  auto results = ParseSimResults(client.SendCommand("SIM item1 10 using=events"));
  ASSERT_EQ(results.size(), 2U);
  EXPECT_EQ(results[0].first, "item2");
  EXPECT_EQ(results[1].first, "item3");

  // 4. SIM search (vectors) - item2 should be most similar to item1
  results = ParseSimResults(client.SendCommand("SIM item1 10 using=vectors"));
  ASSERT_FALSE(results.empty());
  EXPECT_EQ(results[0].first, "item2");

  // 5. INFO should show the seeded counts
  auto resp = client.SendCommand("INFO");
  EXPECT_EQ(ParseResponseField(resp, "vector_count"), "3");
  EXPECT_EQ(ParseResponseField(resp, "event_count"), "3");
}

// ---------------------------------------------------------------------------
// Error handling test
// ---------------------------------------------------------------------------

TEST_F(EndToEndTest, UnknownCommand) {
  TcpClient client("127.0.0.1", port_);
  EXPECT_EQ(client.SendCommand("FOOBAR"), "ERROR Unknown command: FOOBAR\r\n");
}
