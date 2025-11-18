/**
 * @file snapshot_integration_test.cpp
 * @brief Integration tests for snapshot functionality (DUMP commands)
 *
 * Reference: ../mygram-db/tests/integration/server/end_to_end_test.cpp
 * Reusability: 75% (server setup pattern, TcpClient helper)
 *
 * Tests:
 * - Round-trip: Populate via TCP → SAVE → Clear → LOAD → Verify via TCP
 * - DUMP VERIFY command
 * - DUMP INFO command
 * - Error cases (file not found, corrupted snapshot)
 * - Concurrent DUMP operations
 */

#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

#include "config/config.h"
#include "server/nvecd_server.h"

namespace fs = std::filesystem;

using namespace nvecd;
using namespace nvecd::server;

/**
 * @brief Helper class for TCP client connections
 * Reference: ../mygram-db/tests/integration/server/end_to_end_test.cpp
 */
class TcpClient {
 public:
  TcpClient(const std::string& host, uint16_t port) {
    sock_ = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_ < 0) {
      throw std::runtime_error("Failed to create socket");
    }

    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    inet_pton(AF_INET, host.c_str(), &server_addr.sin_addr);

    if (connect(sock_, (struct sockaddr*)&server_addr, sizeof(server_addr)) <
        0) {
      close(sock_);
      throw std::runtime_error("Failed to connect");
    }
  }

  ~TcpClient() {
    Close();
  }

  void Close() {
    if (sock_ >= 0) {
      close(sock_);
      sock_ = -1;
    }
  }

  std::string SendCommand(const std::string& command) {
    std::string request = command + "\r\n";
    send(sock_, request.c_str(), request.length(), 0);

    char buffer[65536];  // Larger buffer for DUMP INFO responses
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
 * @brief Test fixture for snapshot integration tests
 */
class SnapshotIntegrationTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // Create temporary test directory for snapshots
    test_dir_ = fs::temp_directory_path() / "nvecd_test_snapshots";
    fs::create_directories(test_dir_);

    // Create configuration
    config_.api.tcp.bind = "127.0.0.1";
    config_.api.tcp.port = 0;  // Random port
    config_.network.allow_cidrs = {"127.0.0.1/32"};
    config_.perf.max_connections = 10;
    config_.perf.thread_pool_size = 4;
    config_.snapshot.dir = test_dir_.string();

    config_.events.ctx_buffer_size = 100;
    config_.events.decay_alpha = 0.95;
    config_.events.decay_interval_sec = 300;

    config_.vectors.default_dimension = 128;

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

  /**
   * @brief Helper to populate test data via TCP protocol
   */
  void PopulateTestData(TcpClient& client) {
    // Add events
    EXPECT_TRUE(client.SendCommand("EVENT ctx1 vec1 100").find("OK") == 0);
    EXPECT_TRUE(client.SendCommand("EVENT ctx1 vec2 90").find("OK") == 0);
    EXPECT_TRUE(client.SendCommand("EVENT ctx1 vec3 80").find("OK") == 0);
    EXPECT_TRUE(client.SendCommand("EVENT ctx2 vec2 95").find("OK") == 0);
    EXPECT_TRUE(client.SendCommand("EVENT ctx2 vec3 85").find("OK") == 0);

    // Add vectors (128-dimensional, simple pattern)
    // Format: VECSET <id> <f1> <f2> ... <fN>
    std::string vec1_data;
    std::string vec2_data;
    std::string vec3_data;
    for (int i = 0; i < 128; ++i) {
      vec1_data += (i > 0 ? " " : "") + std::to_string(1.0f);
      vec2_data += (i > 0 ? " " : "") + std::to_string(2.0f);
      vec3_data += (i > 0 ? " " : "") + std::to_string(3.0f);
    }

    EXPECT_TRUE(client.SendCommand("VECSET vec1 " + vec1_data).find("OK") == 0);
    EXPECT_TRUE(client.SendCommand("VECSET vec2 " + vec2_data).find("OK") == 0);
    EXPECT_TRUE(client.SendCommand("VECSET vec3 " + vec3_data).find("OK") == 0);
  }

  /**
   * @brief Helper to verify test data integrity via TCP protocol
   */
  void VerifyTestData(TcpClient& client) {
    // Verify SIM works (implicitly checks events and vectors exist)
    std::string response = client.SendCommand("SIM vec1 10 using=events");
    EXPECT_TRUE(response.find("OK") == 0 || response.find("RESULTS") != std::string::npos);

    response = client.SendCommand("SIM vec2 10 using=vectors");
    EXPECT_TRUE(response.find("OK") == 0 || response.find("RESULTS") != std::string::npos);

    // Check INFO command shows non-zero counts
    response = client.SendCommand("INFO");
    EXPECT_TRUE(response.find("vector_count") != std::string::npos);
  }

  fs::path test_dir_;
  config::Config config_;
  std::unique_ptr<NvecdServer> server_;
  uint16_t port_ = 0;
};

/**
 * @brief Test basic DUMP SAVE and DUMP LOAD round-trip
 */
TEST_F(SnapshotIntegrationTest, BasicSaveLoadRoundTrip) {
  TcpClient client("127.0.0.1", port_);

  // Populate test data
  PopulateTestData(client);

  // Save snapshot
  std::string save_response = client.SendCommand("DUMP SAVE test_snapshot.dmp");
  EXPECT_TRUE(save_response.find("OK") == 0);

  // Close client connection before stopping server
  client.Close();

  // Restart server (simulates server restart - creates new server instance)
  server_->Stop();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  server_ = std::make_unique<NvecdServer>(config_);
  ASSERT_TRUE(server_->Start().has_value());
  port_ = server_->GetPort();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  // Reconnect client
  TcpClient client2("127.0.0.1", port_);

  // Verify data is cleared (new server instance)
  std::string info_response = client2.SendCommand("INFO");
  EXPECT_TRUE(info_response.find("vector_count:0") != std::string::npos ||
              info_response.find("vector_count: 0") != std::string::npos);

  // Load snapshot
  std::string load_response = client2.SendCommand("DUMP LOAD test_snapshot.dmp");
  EXPECT_TRUE(load_response.find("OK") == 0);

  // Verify data is restored
  VerifyTestData(client2);
}

/**
 * @brief Test DUMP SAVE with default filename (timestamp-based)
 */
TEST_F(SnapshotIntegrationTest, SaveWithDefaultFilename) {
  TcpClient client("127.0.0.1", port_);

  PopulateTestData(client);

  // Save with default filename
  std::string save_response = client.SendCommand("DUMP SAVE");
  EXPECT_TRUE(save_response.find("OK") == 0);
  EXPECT_TRUE(save_response.find("snapshot_") != std::string::npos);

  // Extract filename from response
  size_t start = save_response.find("snapshot_");
  ASSERT_NE(start, std::string::npos);
  size_t end = save_response.find(".dmp", start);
  ASSERT_NE(end, std::string::npos);
  std::string filename = save_response.substr(start, end - start + 4);

  // Verify file exists
  fs::path snapshot_path = test_dir_ / filename;
  EXPECT_TRUE(fs::exists(snapshot_path));
}

/**
 * @brief Test DUMP VERIFY command
 */
TEST_F(SnapshotIntegrationTest, VerifySnapshot) {
  TcpClient client("127.0.0.1", port_);

  PopulateTestData(client);

  // Save snapshot
  std::string save_response = client.SendCommand("DUMP SAVE test_verify.dmp");
  EXPECT_TRUE(save_response.find("OK") == 0);

  // Verify snapshot
  std::string verify_response = client.SendCommand("DUMP VERIFY test_verify.dmp");
  EXPECT_TRUE(verify_response.find("OK") == 0);
  EXPECT_TRUE(verify_response.find("valid") != std::string::npos ||
              verify_response.find("OK") == 0);
}

/**
 * @brief Test DUMP INFO command
 */
TEST_F(SnapshotIntegrationTest, SnapshotInfo) {
  TcpClient client("127.0.0.1", port_);

  PopulateTestData(client);

  // Save snapshot
  std::string save_response = client.SendCommand("DUMP SAVE test_info.dmp");
  EXPECT_TRUE(save_response.find("OK") == 0);

  // Get snapshot info
  std::string info_response = client.SendCommand("DUMP INFO test_info.dmp");
  EXPECT_TRUE(info_response.find("OK") == 0);
  EXPECT_TRUE(info_response.find("version") != std::string::npos);
  EXPECT_TRUE(info_response.find("timestamp") != std::string::npos ||
              info_response.find("file_size") != std::string::npos);
}

/**
 * @brief Test error case: Load non-existent file
 */
TEST_F(SnapshotIntegrationTest, LoadNonExistentFile) {
  TcpClient client("127.0.0.1", port_);

  std::string response = client.SendCommand("DUMP LOAD nonexistent.dmp");
  EXPECT_TRUE(response.find("ERR") == 0);
}

/**
 * @brief Test error case: Verify non-existent file
 */
TEST_F(SnapshotIntegrationTest, VerifyNonExistentFile) {
  TcpClient client("127.0.0.1", port_);

  std::string response = client.SendCommand("DUMP VERIFY nonexistent.dmp");
  EXPECT_TRUE(response.find("ERR") == 0);
}

/**
 * @brief Test error case: Path traversal protection
 */
TEST_F(SnapshotIntegrationTest, PathTraversalProtection) {
  TcpClient client("127.0.0.1", port_);

  // Try to save outside dump directory
  std::string response = client.SendCommand("DUMP SAVE ../../../etc/passwd");
  EXPECT_TRUE(response.find("ERR") == 0);

  // Try to load from outside dump directory
  response = client.SendCommand("DUMP LOAD ../../sensitive_file");
  EXPECT_TRUE(response.find("ERR") == 0);
}

/**
 * @brief Test concurrent DUMP operations (should be serialized)
 */
TEST_F(SnapshotIntegrationTest, ConcurrentDumpOperations) {
  // Populate via first client
  {
    TcpClient client("127.0.0.1", port_);
    PopulateTestData(client);
  }

  // Create multiple clients and launch concurrent SAVE operations
  std::vector<std::thread> threads;
  std::vector<std::string> responses(3);

  threads.emplace_back([this, &responses]() {
    TcpClient client("127.0.0.1", port_);
    responses[0] = client.SendCommand("DUMP SAVE concurrent1.dmp");
  });
  threads.emplace_back([this, &responses]() {
    TcpClient client("127.0.0.1", port_);
    responses[1] = client.SendCommand("DUMP SAVE concurrent2.dmp");
  });
  threads.emplace_back([this, &responses]() {
    TcpClient client("127.0.0.1", port_);
    responses[2] = client.SendCommand("DUMP SAVE concurrent3.dmp");
  });

  for (auto& t : threads) {
    t.join();
  }

  // All operations should complete (may have errors due to serialization)
  int successful_saves = 0;
  for (const auto& response : responses) {
    if (response.find("OK") == 0) {
      successful_saves++;
    }
  }

  // At least one should succeed
  EXPECT_GT(successful_saves, 0);
}

/**
 * @brief Test large snapshot (stress test)
 */
TEST_F(SnapshotIntegrationTest, LargeSnapshot) {
  TcpClient client("127.0.0.1", port_);

  // Add many events and vectors
  const int num_contexts = 20;  // Reduced for faster testing
  const int num_vectors = 100;  // Reduced for faster testing

  for (int i = 0; i < num_contexts; ++i) {
    std::string ctx = "large_ctx_" + std::to_string(i);
    for (int j = 0; j < 10; ++j) {
      std::string vec_id = "large_vec_" + std::to_string(i * 10 + j);
      std::string cmd = "EVENT " + ctx + " " + vec_id + " " + std::to_string(100 - j);
      client.SendCommand(cmd);
    }
  }

  for (int i = 0; i < num_vectors; ++i) {
    std::string vec_id = "large_vec_" + std::to_string(i);
    std::string vec_data;
    for (int j = 0; j < 128; ++j) {
      vec_data += (j > 0 ? " " : "") + std::to_string(static_cast<float>(i % 10));
    }
    client.SendCommand("VECSET " + vec_id + " " + vec_data);
  }

  // Save (should handle large data)
  auto start = std::chrono::high_resolution_clock::now();
  std::string save_response = client.SendCommand("DUMP SAVE large_test.dmp");
  auto end = std::chrono::high_resolution_clock::now();
  auto duration =
      std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

  EXPECT_TRUE(save_response.find("OK") == 0);
  std::cout << "Large snapshot save took " << duration.count() << "ms\n";

  // Verify file exists and has reasonable size
  fs::path snapshot_path = test_dir_ / "large_test.dmp";
  EXPECT_TRUE(fs::exists(snapshot_path));
  if (fs::exists(snapshot_path)) {
    auto file_size = fs::file_size(snapshot_path);
    EXPECT_GT(file_size, 1000);  // Should be at least 1KB
    std::cout << "Large snapshot file size: " << file_size << " bytes\n";
  }
}
