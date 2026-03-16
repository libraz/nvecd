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

#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <thread>

#include "config/config.h"
#include "server/nvecd_server.h"

namespace fs = std::filesystem;

using namespace nvecd;
using namespace nvecd::server;

/**
 * @brief Helper class for TCP client connections
 * Reference: tests/integration/snapshot/snapshot_integration_test.cpp
 */
class TcpClient {
 public:
  TcpClient(const std::string& host, uint16_t port) {
    sock_ = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_ < 0) {
      throw std::runtime_error("Failed to create socket");
    }

    struct sockaddr_in server_addr {};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    inet_pton(AF_INET, host.c_str(), &server_addr.sin_addr);

    if (connect(sock_, reinterpret_cast<struct sockaddr*>(&server_addr),
                sizeof(server_addr)) < 0) {
      close(sock_);
      throw std::runtime_error("Failed to connect");
    }
  }

  ~TcpClient() { Close(); }

  // Non-copyable
  TcpClient(const TcpClient&) = delete;
  TcpClient& operator=(const TcpClient&) = delete;

  void Close() {
    if (sock_ >= 0) {
      close(sock_);
      sock_ = -1;
    }
  }

  std::string SendCommand(const std::string& command) {
    std::string request = command + "\r\n";
    send(sock_, request.c_str(), request.length(), 0);

    char buffer[65536];
    ssize_t received = recv(sock_, buffer, sizeof(buffer) - 1, 0);
    if (received <= 0) {
      return "";
    }
    buffer[received] = '\0';
    return std::string(buffer);
  }

 private:
  int sock_ = -1;
};

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
  auto resp = client.SendCommand("EVENT ctx1 ADD item1 100");
  EXPECT_TRUE(resp.find("OK") == 0);
}

TEST_F(EndToEndTest, EventAdd_InvalidScore) {
  TcpClient client("127.0.0.1", port_);
  auto resp = client.SendCommand("EVENT ctx1 ADD item1 notanumber");
  EXPECT_TRUE(resp.find("ERROR") != std::string::npos ||
              resp.find("ERR") != std::string::npos);
}

TEST_F(EndToEndTest, EventAdd_MissingArgs) {
  TcpClient client("127.0.0.1", port_);
  auto resp = client.SendCommand("EVENT ctx1 ADD");
  EXPECT_TRUE(resp.find("ERROR") != std::string::npos ||
              resp.find("ERR") != std::string::npos);
}

// ---------------------------------------------------------------------------
// VECSET tests
// ---------------------------------------------------------------------------

TEST_F(EndToEndTest, Vecset_Basic) {
  TcpClient client("127.0.0.1", port_);
  auto resp = client.SendCommand("VECSET item1 1.0 2.0 3.0");
  EXPECT_TRUE(resp.find("OK") == 0);
}

TEST_F(EndToEndTest, Vecset_DimensionMismatch) {
  TcpClient client("127.0.0.1", port_);
  // First set a vector with 3 dimensions
  client.SendCommand("VECSET item1 1.0 2.0 3.0");
  // Try to set a different vector with 2 dimensions (mismatch)
  auto resp = client.SendCommand("VECSET item2 1.0 2.0");
  EXPECT_TRUE(resp.find("ERROR") != std::string::npos ||
              resp.find("ERR") != std::string::npos);
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
  auto resp = client.SendCommand("SIM item1 10 using=events");
  EXPECT_TRUE(resp.find("OK") == 0);
}

TEST_F(EndToEndTest, Sim_VectorBased) {
  TcpClient client("127.0.0.1", port_);
  client.SendCommand("VECSET vec1 1.0 0.0 0.0");
  client.SendCommand("VECSET vec2 0.9 0.1 0.0");
  client.SendCommand("VECSET vec3 0.0 1.0 0.0");
  auto resp = client.SendCommand("SIM vec1 10 using=vectors");
  EXPECT_TRUE(resp.find("OK") == 0);
}

TEST_F(EndToEndTest, Sim_Fusion) {
  TcpClient client("127.0.0.1", port_);
  client.SendCommand("EVENT ctx1 ADD item1 100");
  client.SendCommand("EVENT ctx1 ADD item2 90");
  client.SendCommand("VECSET item1 1.0 0.0 0.0");
  client.SendCommand("VECSET item2 0.9 0.1 0.0");
  auto resp = client.SendCommand("SIM item1 10 using=fusion");
  EXPECT_TRUE(resp.find("OK") == 0);
}

TEST_F(EndToEndTest, Sim_NotFound) {
  TcpClient client("127.0.0.1", port_);
  auto resp = client.SendCommand("SIM nonexistent 10 using=events");
  // Should return OK with 0 results or ERROR
  EXPECT_TRUE(resp.find("OK") == 0 ||
              resp.find("ERROR") != std::string::npos);
}

// ---------------------------------------------------------------------------
// SIMV test
// ---------------------------------------------------------------------------

TEST_F(EndToEndTest, Simv_Basic) {
  TcpClient client("127.0.0.1", port_);
  client.SendCommand("VECSET vec1 1.0 0.0 0.0");
  client.SendCommand("VECSET vec2 0.9 0.1 0.0");
  client.SendCommand("VECSET vec3 0.0 1.0 0.0");
  auto resp = client.SendCommand("SIMV 10 1.0 0.0 0.0");
  EXPECT_TRUE(resp.find("OK") == 0);
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
  EXPECT_TRUE(resp.find("OK INFO") != std::string::npos);
  // Should have non-zero counts
  EXPECT_TRUE(resp.find("vector_count: 0") == std::string::npos ||
              resp.find("vector_count: 1") != std::string::npos);
  EXPECT_TRUE(resp.find("event_count: 0") == std::string::npos ||
              resp.find("event_count: 1") != std::string::npos);
}

// ---------------------------------------------------------------------------
// CONFIG tests
// ---------------------------------------------------------------------------

TEST_F(EndToEndTest, ConfigHelp_Root) {
  TcpClient client("127.0.0.1", port_);
  auto resp = client.SendCommand("CONFIG HELP");
  EXPECT_TRUE(resp.find("+OK") != std::string::npos ||
              resp.find("OK") != std::string::npos);
}

TEST_F(EndToEndTest, ConfigHelp_Path) {
  TcpClient client("127.0.0.1", port_);
  auto resp = client.SendCommand("CONFIG HELP api");
  EXPECT_TRUE(resp.find("+OK") != std::string::npos ||
              resp.find("OK") != std::string::npos ||
              resp.find("-ERR") != std::string::npos);
}

TEST_F(EndToEndTest, ConfigShow_Full) {
  TcpClient client("127.0.0.1", port_);
  auto resp = client.SendCommand("CONFIG SHOW");
  EXPECT_TRUE(resp.find("+OK") != std::string::npos ||
              resp.find("OK") != std::string::npos);
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
  auto resp = client.SendCommand("CACHE CLEAR");
  EXPECT_TRUE(resp.find("OK") == 0);
}

TEST_F(EndToEndTest, CacheEnableDisable) {
  TcpClient client("127.0.0.1", port_);
  auto resp = client.SendCommand("CACHE DISABLE");
  EXPECT_TRUE(resp.find("OK") == 0);
  resp = client.SendCommand("CACHE STATS");
  EXPECT_TRUE(resp.find("cache_enabled: false") != std::string::npos);
  resp = client.SendCommand("CACHE ENABLE");
  EXPECT_TRUE(resp.find("OK") == 0);
  resp = client.SendCommand("CACHE STATS");
  EXPECT_TRUE(resp.find("cache_enabled: true") != std::string::npos);
}

// ---------------------------------------------------------------------------
// DEBUG tests
// ---------------------------------------------------------------------------

TEST_F(EndToEndTest, DebugOn_Off) {
  TcpClient client("127.0.0.1", port_);
  auto resp = client.SendCommand("DEBUG ON");
  EXPECT_TRUE(resp.find("OK") == 0);
  resp = client.SendCommand("DEBUG OFF");
  EXPECT_TRUE(resp.find("OK") == 0);
}

// ---------------------------------------------------------------------------
// VARIABLES tests
// ---------------------------------------------------------------------------

TEST_F(EndToEndTest, SetGet_Variable) {
  TcpClient client("127.0.0.1", port_);
  auto resp = client.SendCommand("SET cache.ttl_seconds 60");
  EXPECT_TRUE(resp.find("+OK") == 0);
  resp = client.SendCommand("GET cache.ttl_seconds");
  EXPECT_TRUE(resp.find("60") != std::string::npos);
}

TEST_F(EndToEndTest, ShowVariables) {
  TcpClient client("127.0.0.1", port_);
  auto resp = client.SendCommand("SHOW VARIABLES");
  // Returns array format: *N\r\n...
  EXPECT_TRUE(resp.find("*") == 0);
}

// ---------------------------------------------------------------------------
// Full pipeline workflow test
// ---------------------------------------------------------------------------

TEST_F(EndToEndTest, FullPipeline) {
  TcpClient client("127.0.0.1", port_);

  // 1. Add events
  EXPECT_TRUE(client.SendCommand("EVENT ctx1 ADD item1 100").find("OK") == 0);
  EXPECT_TRUE(client.SendCommand("EVENT ctx1 ADD item2 90").find("OK") == 0);
  EXPECT_TRUE(client.SendCommand("EVENT ctx1 ADD item3 80").find("OK") == 0);

  // 2. Set vectors
  EXPECT_TRUE(client.SendCommand("VECSET item1 1.0 0.0 0.0").find("OK") == 0);
  EXPECT_TRUE(client.SendCommand("VECSET item2 0.9 0.1 0.0").find("OK") == 0);
  EXPECT_TRUE(client.SendCommand("VECSET item3 0.0 1.0 0.0").find("OK") == 0);

  // 3. SIM search (events)
  auto resp = client.SendCommand("SIM item1 10 using=events");
  EXPECT_TRUE(resp.find("OK") == 0);

  // 4. SIM search (vectors) - item2 should be most similar to item1
  resp = client.SendCommand("SIM item1 10 using=vectors");
  EXPECT_TRUE(resp.find("OK") == 0);
  EXPECT_TRUE(resp.find("RESULTS") != std::string::npos);

  // 5. INFO should show non-zero counts
  resp = client.SendCommand("INFO");
  EXPECT_TRUE(resp.find("vector_count: 3") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Error handling test
// ---------------------------------------------------------------------------

TEST_F(EndToEndTest, UnknownCommand) {
  TcpClient client("127.0.0.1", port_);
  auto resp = client.SendCommand("FOOBAR");
  EXPECT_TRUE(resp.find("ERROR") != std::string::npos ||
              resp.find("ERR") != std::string::npos);
}
