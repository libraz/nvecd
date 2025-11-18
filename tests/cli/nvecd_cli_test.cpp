/**
 * @file nvecd_cli_test.cpp
 * @brief Integration tests for nvecd-cli
 *
 * Reference: tests/integration/snapshot/snapshot_integration_test.cpp
 * Reusability: 85% (server setup pattern, TcpClient helper from snapshot test)
 *
 * Tests CLI tool by running actual nvecd server instance and CLI subprocess
 */

#include <gtest/gtest.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>

#include "config/config.h"
#include "server/nvecd_server.h"

namespace nvecd {
namespace cli {
namespace test {

namespace {

/**
 * @brief Execute shell command and capture output
 */
std::string ExecuteCommand(const std::string& command) {
  std::array<char, 4096> buffer{};
  std::string result;
  std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(command.c_str(), "r"), pclose);
  if (!pipe) {
    return "";
  }
  while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
    result += buffer.data();
  }
  return result;
}

}  // namespace

/**
 * @brief Test fixture for CLI integration tests
 * Reference: tests/integration/snapshot/snapshot_integration_test.cpp
 */
class NvecdCliTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // Create configuration (same pattern as snapshot_integration_test.cpp)
    config_.api.tcp.bind = "127.0.0.1";
    config_.api.tcp.port = 0;  // Random port
    config_.network.allow_cidrs = {"127.0.0.1/32"};
    config_.perf.max_connections = 10;
    config_.perf.thread_pool_size = 4;

    config_.events.ctx_buffer_size = 100;
    config_.events.decay_alpha = 0.95;
    config_.events.decay_interval_sec = 300;

    config_.vectors.default_dimension = 4;  // Small for testing

    config_.similarity.default_top_k = 10;
    config_.similarity.max_top_k = 100;
    config_.similarity.fusion_alpha = 0.5;
    config_.similarity.fusion_beta = 0.5;

    config_.cache.enabled = true;
    config_.cache.max_memory_bytes = 16 * 1024 * 1024;  // 16MB

    // Create and start server
    server_ = std::make_unique<server::NvecdServer>(config_);
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
  }

  std::string RunCli(const std::string& command) {
    // CLI binary is in build/bin/, tests run from build/tests/
    // Use printf to send command + quit in interactive mode
    // This ensures the CLI properly processes the command and exits cleanly
    std::string full_command =
        "printf '" + command + "\\nquit\\n' | ../bin/nvecd-cli -h 127.0.0.1 -p " + std::to_string(port_) + " 2>&1";
    return ExecuteCommand(full_command);
  }

  config::Config config_;
  std::unique_ptr<server::NvecdServer> server_;
  uint16_t port_{0};
};

/**
 * @brief Test INFO command via CLI
 */
TEST_F(NvecdCliTest, InfoCommand) {
  std::string output = RunCli("INFO");

  EXPECT_NE(output.find("# Server"), std::string::npos);
  EXPECT_NE(output.find("version:"), std::string::npos);
  EXPECT_NE(output.find("uptime_seconds:"), std::string::npos);
}

/**
 * @brief Test VECSET command via CLI
 */
TEST_F(NvecdCliTest, VecsetCommand) {
  std::string output = RunCli("VECSET item1 0.1 0.2 0.3 0.4");

  // Debug: print what we actually got
  std::cerr << "=== VECSET Test Output ===" << std::endl;
  std::cerr << "Length: " << output.length() << std::endl;
  std::cerr << "Content: [" << output << "]" << std::endl;
  std::cerr << "=========================" << std::endl;

  // Should get OK response from server
  EXPECT_TRUE(output.find("OK") != std::string::npos);
}

/**
 * @brief Test EVENT command via CLI
 */
TEST_F(NvecdCliTest, EventCommand) {
  std::string output = RunCli("EVENT user1 item1 100");

  // Should get OK response from server
  EXPECT_TRUE(output.find("OK") != std::string::npos);
}

/**
 * @brief Test SIM command via CLI
 */
TEST_F(NvecdCliTest, SimCommand) {
  // Register vectors first
  RunCli("VECSET item1 0.1 0.2 0.3 0.4");
  RunCli("VECSET item2 0.15 0.25 0.28 0.38");

  std::string output = RunCli("SIM item1 10 using=vectors");

  EXPECT_TRUE(output.find("item") != std::string::npos || output.find("OK") != std::string::npos);
}

/**
 * @brief Test CACHE STATS command via CLI
 */
TEST_F(NvecdCliTest, CacheStatsCommand) {
  std::string output = RunCli("CACHE STATS");

  EXPECT_TRUE(output.find("total_queries") != std::string::npos || output.find("OK") != std::string::npos);
}

}  // namespace test
}  // namespace cli
}  // namespace nvecd
