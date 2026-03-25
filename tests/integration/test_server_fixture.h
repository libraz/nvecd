/**
 * @file test_server_fixture.h
 * @brief Shared test fixture for nvecd integration tests
 */

#pragma once

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

#include "config/config.h"
#include "server/nvecd_server.h"
#include "test_tcp_client.h"

namespace fs = std::filesystem;

/**
 * @brief Base fixture for nvecd integration tests
 *
 * Provides server lifecycle management with configurable vector dimension.
 * Uses random port assignment for test isolation.
 */
class NvecdTestFixture : public ::testing::Test {
 protected:
  void SetUpServer(int dimension = 3) {
    test_dir_ = fs::temp_directory_path() / ("nvecd_test_" + std::to_string(getpid()));
    fs::create_directories(test_dir_);

    config_.api.tcp.bind = "127.0.0.1";
    config_.api.tcp.port = 0;  // Random port
    config_.network.allow_cidrs = {"127.0.0.1/32"};
    config_.perf.max_connections = 10;
    config_.perf.thread_pool_size = 4;
    config_.snapshot.dir = test_dir_.string();

    config_.events.ctx_buffer_size = 100;
    config_.events.decay_alpha = 0.95;
    config_.events.decay_interval_sec = 300;

    config_.vectors.default_dimension = static_cast<uint32_t>(dimension);

    config_.similarity.default_top_k = 10;
    config_.similarity.max_top_k = 100;
    config_.similarity.fusion_alpha = 0.5;
    config_.similarity.fusion_beta = 0.5;

    server_ = std::make_unique<nvecd::server::NvecdServer>(config_);
    ASSERT_TRUE(server_->Start().has_value());

    port_ = server_->GetPort();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  void TearDownServer() {
    if (server_) {
      server_->Stop();
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (fs::exists(test_dir_)) {
      fs::remove_all(test_dir_);
    }
  }

  /**
   * @brief Populate 3 items with events and 3-dimensional vectors
   */
  void PopulateBasicData(TcpClient& client) {
    EXPECT_TRUE(ContainsOK(client.SendCommand("EVENT ctx1 ADD item1 100")));
    EXPECT_TRUE(ContainsOK(client.SendCommand("EVENT ctx1 ADD item2 90")));
    EXPECT_TRUE(ContainsOK(client.SendCommand("EVENT ctx1 ADD item3 80")));
    EXPECT_TRUE(ContainsOK(client.SendCommand("VECSET item1 1.0 0.0 0.0")));
    EXPECT_TRUE(ContainsOK(client.SendCommand("VECSET item2 0.9 0.1 0.0")));
    EXPECT_TRUE(ContainsOK(client.SendCommand("VECSET item3 0.0 1.0 0.0")));
  }

  fs::path test_dir_;
  nvecd::config::Config config_;
  std::unique_ptr<nvecd::server::NvecdServer> server_;
  uint16_t port_ = 0;
};
