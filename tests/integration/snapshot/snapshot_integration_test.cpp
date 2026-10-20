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

#include <gtest/gtest.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

#include "../test_tcp_client.h"
#include "config/config.h"
#include "server/nvecd_server.h"

namespace fs = std::filesystem;

using namespace nvecd;
using namespace nvecd::server;

namespace {

bool EndsWith(const std::string& text, const std::string& suffix) {
  return text.size() >= suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

/// Ids of a SIM reply in rank order.
std::vector<std::string> SimIds(const std::string& response) {
  std::vector<std::string> ids;
  for (const auto& [id, score] : ParseSimResults(response)) {
    ids.push_back(id);
  }
  return ids;
}

}  // namespace

/**
 * @brief Test fixture for snapshot integration tests
 */
class SnapshotIntegrationTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // Give every case its own snapshot directory. Cases run as separate
    // processes, so a shared fixed path lets one case's TearDown() delete the
    // directory another case is still writing snapshots into.
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    test_dir_ = fs::temp_directory_path() / ("nvecd_test_snapshots_" + std::to_string(::getpid()) + "_" + info->name());
    fs::remove_all(test_dir_);
    fs::create_directories(test_dir_);

    // Create configuration
    config_.api.tcp.bind = "127.0.0.1";
    config_.api.tcp.port = 0;  // Random port
    config_.network.allow_cidrs = {"127.0.0.1/32"};
    config_.perf.max_connections = 10;
    config_.perf.thread_pool_size = 4;
    config_.snapshot.dir = test_dir_.string();
    config_.snapshot.mode = "lock";  // Use synchronous mode for test determinism

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
    EXPECT_TRUE(client.SendCommand("EVENT ctx1 ADD vec1 100").find("OK") == 0);
    EXPECT_TRUE(client.SendCommand("EVENT ctx1 ADD vec2 90").find("OK") == 0);
    EXPECT_TRUE(client.SendCommand("EVENT ctx1 ADD vec3 80").find("OK") == 0);
    EXPECT_TRUE(client.SendCommand("EVENT ctx2 ADD vec2 95").find("OK") == 0);
    EXPECT_TRUE(client.SendCommand("EVENT ctx2 ADD vec3 85").find("OK") == 0);

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
    EXPECT_TRUE(client.SendCommand("METASET vec2 status:active,category:test").find("OK") == 0);
    EXPECT_TRUE(client.SendCommand("METASET vec3 status:draft,category:test").find("OK") == 0);
  }

  /**
   * @brief Helper to verify test data integrity via TCP protocol
   */
  void VerifyTestData(TcpClient& client) {
    // Co-occurrence: vec1 pairs with vec2 at 100*90 and with vec3 at 100*80.
    EXPECT_EQ(SimIds(client.SendCommand("SIM vec1 10 using=events")), (std::vector<std::string>{"vec2", "vec3"}));

    // The three vectors are parallel, so both others are returned.
    auto vector_ids = SimIds(client.SendCommand("SIM vec2 10 using=vectors"));
    std::sort(vector_ids.begin(), vector_ids.end());
    EXPECT_EQ(vector_ids, (std::vector<std::string>{"vec1", "vec3"}));

    // Metadata survived: only vec2 is active.
    EXPECT_EQ(SimIds(client.SendCommand("SIM vec1 10 using=vectors filter=status:active")),
              (std::vector<std::string>{"vec2"}));

    // INFO reports the seeded counts
    std::string info = client.SendCommand("INFO");
    EXPECT_EQ(ParseResponseField(info, "vector_count"), "3");
    EXPECT_EQ(ParseResponseField(info, "event_count"), "5");
  }

  /**
   * @brief Wait for background fork snapshot to complete via DUMP STATUS polling
   * @param client TCP client to use for polling
   * @param timeout_ms Maximum wait time in milliseconds
   * @return true if snapshot completed, false if timed out
   */
  bool WaitForSnapshotComplete(TcpClient& client, int timeout_ms = 5000) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
      std::string status = client.SendCommand("DUMP STATUS");
      // "idle" or "completed" means snapshot is no longer in progress
      if (status.find("idle") != std::string::npos || status.find("completed") != std::string::npos) {
        return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return false;
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
  ASSERT_TRUE(WaitForSnapshotComplete(client)) << "Snapshot did not complete in time";

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

  // WAL-off is the default configuration. A durable DUMP SAVE must therefore
  // be recovered automatically on restart even though it has no .walseq
  // sidecar; otherwise the default deployment always comes back empty.
  VerifyTestData(client2);

  // An explicit load remains safe and idempotent after automatic recovery.
  std::string load_response = client2.SendCommand("DUMP LOAD test_snapshot.dmp");
  EXPECT_TRUE(load_response.find("OK") == 0);

  // Verify data is restored
  VerifyTestData(client2);
}

/**
 * @brief Test DUMP SAVE with no argument, which uses snapshot.default_filename
 */
TEST_F(SnapshotIntegrationTest, SaveWithDefaultFilename) {
  TcpClient client("127.0.0.1", port_);

  PopulateTestData(client);

  // Save with no path: the destination is the configured default filename.
  std::string save_response = client.SendCommand("DUMP SAVE");
  EXPECT_TRUE(save_response.find("OK") == 0);
  EXPECT_NE(save_response.find(config_.snapshot.default_filename), std::string::npos) << save_response;

  ASSERT_TRUE(WaitForSnapshotComplete(client)) << "Snapshot did not complete in time";

  EXPECT_TRUE(fs::exists(test_dir_ / config_.snapshot.default_filename));
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
  ASSERT_TRUE(WaitForSnapshotComplete(client)) << "Snapshot did not complete in time";

  // Verify snapshot
  std::string verify_response = client.SendCommand("DUMP VERIFY test_verify.dmp");
  EXPECT_EQ(verify_response.find("OK DUMP_VERIFIED "), 0U) << verify_response;
  EXPECT_TRUE(EndsWith(verify_response, "test_verify.dmp\r\n")) << verify_response;
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
  ASSERT_TRUE(WaitForSnapshotComplete(client)) << "Snapshot did not complete in time";

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
  EXPECT_TRUE(ContainsError(response)) << response;
}

/**
 * @brief Test error case: Verify non-existent file
 */
TEST_F(SnapshotIntegrationTest, VerifyNonExistentFile) {
  TcpClient client("127.0.0.1", port_);

  std::string response = client.SendCommand("DUMP VERIFY nonexistent.dmp");
  EXPECT_TRUE(ContainsError(response)) << response;
}

/**
 * @brief Test error case: Path traversal protection
 */
TEST_F(SnapshotIntegrationTest, PathTraversalProtection) {
  TcpClient client("127.0.0.1", port_);

  // Try to save outside dump directory
  std::string response = client.SendCommand("DUMP SAVE ../../../etc/passwd");
  EXPECT_TRUE(ContainsError(response)) << response;

  // Try to load from outside dump directory
  response = client.SendCommand("DUMP LOAD ../../sensitive_file");
  EXPECT_TRUE(ContainsError(response)) << response;
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
      std::string cmd = "EVENT " + ctx + " ADD " + vec_id + " " + std::to_string(100 - j);
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
  auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

  EXPECT_TRUE(save_response.find("OK") == 0);
  ASSERT_TRUE(WaitForSnapshotComplete(client)) << "Snapshot did not complete in time";
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
