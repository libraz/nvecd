/**
 * @file nvecdclient_c_test.cpp
 * @brief Unit tests for nvecd C API client library
 *
 * Reference: ../mygram-db/tests/client/mygramclient_c_test.cpp (patterns)
 */

#include "client/nvecdclient_c.h"

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <thread>
#include <vector>

#include "config/config.h"
#include "server/nvecd_server.h"

using namespace nvecd;

namespace {

constexpr uint16_t kTestPort = 0;  // Let OS assign random port

/**
 * @brief Test fixture with embedded server
 */
class NvecdClientCTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // Create config
    config_ = std::make_unique<config::Config>();
    config_->api.tcp.port = kTestPort;
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
    ASSERT_TRUE(start_result) << "Failed to start server";

    port_ = server_->GetPort();
    ASSERT_GT(port_, 0);

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

TEST_F(NvecdClientCTest, CreateAndConnect) {
  NvecdClientConfig_C config = {};
  config.host = "127.0.0.1";
  config.port = port_;
  config.timeout_ms = 5000;            // NOLINT
  config.recv_buffer_size = 65536;     // NOLINT

  NvecdClient_C* client = nvecdclient_create(&config);
  ASSERT_NE(client, nullptr);

  int result = nvecdclient_connect(client);
  EXPECT_EQ(result, 0) << "Connect failed: " << nvecdclient_get_last_error(client);
  EXPECT_EQ(nvecdclient_is_connected(client), 1);

  nvecdclient_disconnect(client);
  EXPECT_EQ(nvecdclient_is_connected(client), 0);

  nvecdclient_destroy(client);
}

//
// EVENT command tests
//

TEST_F(NvecdClientCTest, EventCommand) {
  NvecdClientConfig_C config = {};
  config.host = "127.0.0.1";
  config.port = port_;

  NvecdClient_C* client = nvecdclient_create(&config);
  ASSERT_NE(client, nullptr);
  ASSERT_EQ(nvecdclient_connect(client), 0);

  int result = nvecdclient_event(client, "ctx123", "ADD", "vec456", 95);  // NOLINT
  EXPECT_EQ(result, 0) << "Event failed: " << nvecdclient_get_last_error(client);

  nvecdclient_destroy(client);
}

//
// VECSET command tests
//

TEST_F(NvecdClientCTest, VecsetCommand) {
  NvecdClientConfig_C config = {};
  config.host = "127.0.0.1";
  config.port = port_;

  NvecdClient_C* client = nvecdclient_create(&config);
  ASSERT_NE(client, nullptr);
  ASSERT_EQ(nvecdclient_connect(client), 0);

  float vector[] = {0.1F, 0.2F, 0.3F};  // NOLINT
  int result = nvecdclient_vecset(client, "vec1", vector, 3);
  EXPECT_EQ(result, 0) << "Vecset failed: " << nvecdclient_get_last_error(client);

  nvecdclient_destroy(client);
}

//
// SIM command tests
//

TEST_F(NvecdClientCTest, SimCommand) {
  NvecdClientConfig_C config = {};
  config.host = "127.0.0.1";
  config.port = port_;

  NvecdClient_C* client = nvecdclient_create(&config);
  ASSERT_NE(client, nullptr);
  ASSERT_EQ(nvecdclient_connect(client), 0);

  // Register vectors
  float vec1[] = {1.0F, 0.0F, 0.0F};  // NOLINT
  float vec2[] = {0.9F, 0.1F, 0.0F};  // NOLINT
  ASSERT_EQ(nvecdclient_vecset(client, "vec1", vec1, 3), 0);
  ASSERT_EQ(nvecdclient_vecset(client, "vec2", vec2, 3), 0);

  // Search
  NvecdSimResponse_C* response = nullptr;
  int result = nvecdclient_sim(client, "vec1", 10, "vectors", &response);  // NOLINT
  ASSERT_EQ(result, 0) << "Sim failed: " << nvecdclient_get_last_error(client);
  ASSERT_NE(response, nullptr);

  EXPECT_GE(response->count, 1);
  EXPECT_STREQ(response->mode, "vectors");
  if (response->count > 0) {
    EXPECT_STREQ(response->results[0].id, "vec2");  // vec2 should be most similar (self is skipped)
  }

  nvecdclient_free_sim_response(response);
  nvecdclient_destroy(client);
}

//
// SIMV command tests
//

TEST_F(NvecdClientCTest, SimvCommand) {
  NvecdClientConfig_C config = {};
  config.host = "127.0.0.1";
  config.port = port_;

  NvecdClient_C* client = nvecdclient_create(&config);
  ASSERT_NE(client, nullptr);
  ASSERT_EQ(nvecdclient_connect(client), 0);

  // Register vector
  float vec1[] = {1.0F, 0.0F, 0.0F};  // NOLINT
  ASSERT_EQ(nvecdclient_vecset(client, "vec1", vec1, 3), 0);

  // Search by vector
  float query[] = {0.9F, 0.1F, 0.0F};  // NOLINT
  NvecdSimResponse_C* response = nullptr;
  int result = nvecdclient_simv(client, query, 3, 10, "vectors", &response);  // NOLINT
  ASSERT_EQ(result, 0) << "Simv failed: " << nvecdclient_get_last_error(client);
  ASSERT_NE(response, nullptr);

  EXPECT_GE(response->count, 1);
  if (response->count > 0) {
    EXPECT_STREQ(response->results[0].id, "vec1");
  }

  nvecdclient_free_sim_response(response);
  nvecdclient_destroy(client);
}

//
// INFO command tests
//

TEST_F(NvecdClientCTest, InfoCommand) {
  NvecdClientConfig_C config = {};
  config.host = "127.0.0.1";
  config.port = port_;

  NvecdClient_C* client = nvecdclient_create(&config);
  ASSERT_NE(client, nullptr);
  ASSERT_EQ(nvecdclient_connect(client), 0);

  NvecdServerInfo_C* info = nullptr;
  int result = nvecdclient_info(client, &info);
  ASSERT_EQ(result, 0) << "Info failed: " << nvecdclient_get_last_error(client);
  ASSERT_NE(info, nullptr);

  EXPECT_NE(info->version, nullptr);
  EXPECT_GE(info->uptime_seconds, 0);

  nvecdclient_free_server_info(info);
  nvecdclient_destroy(client);
}

//
// CONFIG command tests
//

TEST_F(NvecdClientCTest, GetConfigCommand) {
  NvecdClientConfig_C config = {};
  config.host = "127.0.0.1";
  config.port = port_;

  NvecdClient_C* client = nvecdclient_create(&config);
  ASSERT_NE(client, nullptr);
  ASSERT_EQ(nvecdclient_connect(client), 0);

  char* config_str = nullptr;
  int result = nvecdclient_get_config(client, &config_str);
  ASSERT_EQ(result, 0) << "GetConfig failed: " << nvecdclient_get_last_error(client);
  ASSERT_NE(config_str, nullptr);

  nvecdclient_free_string(config_str);
  nvecdclient_destroy(client);
}

//
// DEBUG commands tests
//

TEST_F(NvecdClientCTest, DebugCommands) {
  NvecdClientConfig_C config = {};
  config.host = "127.0.0.1";
  config.port = port_;

  NvecdClient_C* client = nvecdclient_create(&config);
  ASSERT_NE(client, nullptr);
  ASSERT_EQ(nvecdclient_connect(client), 0);

  EXPECT_EQ(nvecdclient_debug_on(client), 0);
  EXPECT_EQ(nvecdclient_debug_off(client), 0);

  nvecdclient_destroy(client);
}

//
// Memory management tests
//

TEST_F(NvecdClientCTest, MemoryManagement) {
  // Ensure all free functions handle NULL gracefully
  nvecdclient_free_sim_response(nullptr);
  nvecdclient_free_server_info(nullptr);
  nvecdclient_free_string(nullptr);
  // Should not crash
}
