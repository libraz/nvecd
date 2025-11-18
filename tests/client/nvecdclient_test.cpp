/**
 * @file nvecdclient_test.cpp
 * @brief Unit tests for nvecd C++ client library
 *
 * Reference: ../mygram-db/tests/client/mygramclient_test.cpp (patterns)
 * Note: This is an E2E integration test that requires a running nvecd server
 */

#include "client/nvecdclient.h"

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <thread>

#include "config/config.h"
#include "server/nvecd_server.h"

using namespace nvecd;
using namespace nvecd::client;
using namespace nvecd::utils;

namespace {

// Test server port (random high port to avoid conflicts)
constexpr uint16_t kTestPort = 0;  // Let OS assign random port

/**
 * @brief Test fixture with embedded server
 */
class NvecdClientTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // Create config
    config_ = std::make_unique<config::Config>();
    config_->api.tcp.port = kTestPort;  // Random port
    config_->api.http.enable = false;
    config_->perf.thread_pool_size = 2;      // NOLINT
    config_->events.ctx_buffer_size = 100;          // NOLINT
    config_->events.decay_interval_sec = 60;        // NOLINT
    config_->events.decay_alpha = 0.9;              // NOLINT
    config_->vectors.default_dimension = 3;         // Small dimension for tests
    config_->network.allow_cidrs = {"127.0.0.1/32"};  // Allow localhost
    config_->snapshot.dir = "/tmp/nvecd_test_snapshots";  // Use temp dir for tests

    // Create and start server (server owns stores)
    server_ = std::make_unique<server::NvecdServer>(*config_);
    auto start_result = server_->Start();
    ASSERT_TRUE(start_result) << "Failed to start server: " << start_result.error().message();

    // Get actual port (if we used port 0)
    port_ = server_->GetPort();
    ASSERT_GT(port_, 0) << "Server port is invalid";

    // Small delay to ensure server is ready
    std::this_thread::sleep_for(std::chrono::milliseconds(100));  // NOLINT
  }

  void TearDown() override {
    if (server_) {
      server_->Stop();
    }
  }

  std::unique_ptr<config::Config> config_;
  std::unique_ptr<server::NvecdServer> server_;
  uint16_t port_ = 0;
};

}  // namespace

//
// Connection tests
//

TEST_F(NvecdClientTest, ConnectSuccess) {
  ClientConfig config;
  config.host = "127.0.0.1";
  config.port = port_;

  NvecdClient client(config);
  auto result = client.Connect();
  ASSERT_TRUE(result) << "Connect failed: " << result.error().message();
  EXPECT_TRUE(client.IsConnected());

  client.Disconnect();
  EXPECT_FALSE(client.IsConnected());
}

TEST_F(NvecdClientTest, ConnectInvalidPort) {
  ClientConfig config;
  config.host = "127.0.0.1";
  config.port = 1;  // Invalid port

  NvecdClient client(config);
  auto result = client.Connect();
  EXPECT_FALSE(result);
  EXPECT_FALSE(client.IsConnected());
}

//
// EVENT command tests
//

TEST_F(NvecdClientTest, EventSuccess) {
  ClientConfig config;
  config.host = "127.0.0.1";
  config.port = port_;

  NvecdClient client(config);
  ASSERT_TRUE(client.Connect());

  auto result = client.Event("ctx123", "ADD", "vec456", 95);  // NOLINT
  EXPECT_TRUE(result) << "Event failed: " << result.error().message();
}

TEST_F(NvecdClientTest, EventInvalidScore) {
  ClientConfig config;
  config.host = "127.0.0.1";
  config.port = port_;

  NvecdClient client(config);
  ASSERT_TRUE(client.Connect());

  auto result = client.Event("ctx123", "ADD", "vec456", 150);  // NOLINT - invalid score
  EXPECT_FALSE(result);
}

//
// VECSET command tests
//

TEST_F(NvecdClientTest, VecsetSuccess) {
  ClientConfig config;
  config.host = "127.0.0.1";
  config.port = port_;

  NvecdClient client(config);
  ASSERT_TRUE(client.Connect());

  std::vector<float> vec = {0.1F, 0.2F, 0.3F};
  auto result = client.Vecset("vec1", vec);
  EXPECT_TRUE(result) << "Vecset failed: " << result.error().message();
}

TEST_F(NvecdClientTest, VecsetEmptyVector) {
  ClientConfig config;
  config.host = "127.0.0.1";
  config.port = port_;

  NvecdClient client(config);
  ASSERT_TRUE(client.Connect());

  std::vector<float> vec;
  auto result = client.Vecset("vec1", vec);
  EXPECT_FALSE(result);
}

//
// SIM command tests
//

TEST_F(NvecdClientTest, SimSuccess) {
  ClientConfig config;
  config.host = "127.0.0.1";
  config.port = port_;

  NvecdClient client(config);
  ASSERT_TRUE(client.Connect());

  // Register vectors
  std::vector<float> vec1 = {1.0F, 0.0F, 0.0F};
  std::vector<float> vec2 = {0.9F, 0.1F, 0.0F};  // NOLINT
  auto vec1_result = client.Vecset("vec1", vec1);
  ASSERT_TRUE(vec1_result) << "Vecset vec1 failed: " << vec1_result.error().message();

  auto vec2_result = client.Vecset("vec2", vec2);
  ASSERT_TRUE(vec2_result) << "Vecset vec2 failed: " << vec2_result.error().message();

  // Search
  auto result = client.Sim("vec1", 10, "vectors");  // NOLINT
  ASSERT_TRUE(result) << "Sim failed: " << result.error().message();

  EXPECT_EQ(result->mode, "vectors");
  EXPECT_GE(result->results.size(), 1) << "Expected at least vec2 in results";
  if (!result->results.empty()) {
    EXPECT_EQ(result->results[0].id, "vec2");  // vec2 should be most similar (self is skipped)
  }
}

TEST_F(NvecdClientTest, SimNonExistentId) {
  ClientConfig config;
  config.host = "127.0.0.1";
  config.port = port_;

  NvecdClient client(config);
  ASSERT_TRUE(client.Connect());

  auto result = client.Sim("nonexistent", 10, "vectors");  // NOLINT
  EXPECT_FALSE(result);  // Should fail for non-existent ID
}

//
// SIMV command tests
//

TEST_F(NvecdClientTest, SimvSuccess) {
  ClientConfig config;
  config.host = "127.0.0.1";
  config.port = port_;

  NvecdClient client(config);
  ASSERT_TRUE(client.Connect());

  // Register vectors
  std::vector<float> vec1 = {1.0F, 0.0F, 0.0F};
  auto vec1_result = client.Vecset("vec1", vec1);
  ASSERT_TRUE(vec1_result) << "Vecset vec1 failed: " << vec1_result.error().message();

  // Search by vector
  std::vector<float> query = {0.9F, 0.1F, 0.0F};  // NOLINT
  auto result = client.Simv(query, 10, "vectors");           // NOLINT
  ASSERT_TRUE(result) << "Simv failed: " << result.error().message();

  EXPECT_EQ(result->mode, "vectors");
  EXPECT_GE(result->results.size(), 1);
  if (!result->results.empty()) {
    EXPECT_EQ(result->results[0].id, "vec1");
  }
}

//
// INFO command tests
//

TEST_F(NvecdClientTest, InfoSuccess) {
  ClientConfig config;
  config.host = "127.0.0.1";
  config.port = port_;

  NvecdClient client(config);
  ASSERT_TRUE(client.Connect());

  auto result = client.Info();
  ASSERT_TRUE(result) << "Info failed: " << result.error().message();

  EXPECT_FALSE(result->version.empty());
  EXPECT_GE(result->uptime_seconds, 0);
}

//
// CONFIG command tests
//

TEST_F(NvecdClientTest, GetConfigSuccess) {
  ClientConfig config;
  config.host = "127.0.0.1";
  config.port = port_;

  NvecdClient client(config);
  ASSERT_TRUE(client.Connect());

  auto result = client.GetConfig();
  ASSERT_TRUE(result) << "GetConfig failed: " << result.error().message();
  EXPECT_FALSE(result->empty());
}

//
// DEBUG command tests
//

TEST_F(NvecdClientTest, DebugCommands) {
  ClientConfig config;
  config.host = "127.0.0.1";
  config.port = port_;

  NvecdClient client(config);
  ASSERT_TRUE(client.Connect());

  auto enable_result = client.EnableDebug();
  EXPECT_TRUE(enable_result) << "EnableDebug failed: " << enable_result.error().message();

  auto disable_result = client.DisableDebug();
  EXPECT_TRUE(disable_result) << "DisableDebug failed: " << disable_result.error().message();
}

//
// DUMP command tests (basic smoke tests)
//

TEST_F(NvecdClientTest, DumpCommandsBasic) {
  ClientConfig config;
  config.host = "127.0.0.1";
  config.port = port_;

  NvecdClient client(config);
  ASSERT_TRUE(client.Connect());

  // Save with default filename
  auto save_result = client.Save("");
  ASSERT_TRUE(save_result) << "Save failed: " << save_result.error().message();
  EXPECT_FALSE(save_result->empty());
}
