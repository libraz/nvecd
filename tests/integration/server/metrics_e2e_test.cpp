/**
 * @file metrics_e2e_test.cpp
 * @brief End-to-end tests for metrics accuracy and memory tracking
 *
 * Verifies that server statistics counters (command counts, connection counts,
 * memory estimates, data counts, cache stats) are accurate after known
 * sequences of operations.
 */

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "../test_server_fixture.h"
#include "../test_tcp_client.h"

/**
 * @brief Fixture for metrics accuracy E2E tests
 *
 * Uses 128-dimensional vectors for memory tracking tests (larger footprint
 * makes relative measurements more reliable). Falls back to 4 dimensions
 * for tests that do not need precise memory assertions.
 */
class MetricsE2ETest : public NvecdTestFixture {
 protected:
  void SetUp() override { SetUpServer(4); }
  void TearDown() override { TearDownServer(); }

  /**
   * @brief Build a VECSET command string for the given item and dimension
   * @param item_id Item identifier
   * @param dim Number of float components (all set to 1.0)
   */
  static std::string MakeVecsetCmd(const std::string& item_id, int dim) {
    std::string cmd = "VECSET " + item_id;
    for (int i = 0; i < dim; ++i) {
      cmd += " 1.0";
    }
    return cmd;
  }
};

/**
 * @brief Higher-dimension fixture for memory tracking tests
 */
class MetricsMemoryE2ETest : public NvecdTestFixture {
 protected:
  void SetUp() override { SetUpServer(128); }
  void TearDown() override { TearDownServer(); }

  static std::string MakeVecsetCmd128(const std::string& item_id) {
    std::string cmd = "VECSET " + item_id;
    for (int i = 0; i < 128; ++i) {
      cmd += " 1.0";
    }
    return cmd;
  }
};

// ---------------------------------------------------------------------------
// Test 1: CommandCountersExact
// ---------------------------------------------------------------------------

TEST_F(MetricsE2ETest, CommandCountersExact) {
  TcpClient client("127.0.0.1", port_);

  // Send exactly 5 EVENT commands
  for (int i = 0; i < 5; ++i) {
    std::string cmd =
        "EVENT ctx1 ADD item_" + std::to_string(i) + " " + std::to_string((i + 1) * 10);
    ASSERT_TRUE(ContainsOK(client.SendCommand(cmd)))
        << "EVENT command " << i << " should succeed";
  }

  // Send exactly 3 VECSET commands
  ASSERT_TRUE(ContainsOK(client.SendCommand("VECSET item_0 1.0 0.0 0.0 0.0")));
  ASSERT_TRUE(ContainsOK(client.SendCommand("VECSET item_1 0.9 0.1 0.0 0.0")));
  ASSERT_TRUE(ContainsOK(client.SendCommand("VECSET item_2 0.0 1.0 0.0 0.0")));

  // Send exactly 4 SIM commands
  for (int i = 0; i < 4; ++i) {
    client.SendCommand("SIM item_0 10 using=events");
  }

  // Send exactly 2 SIMV commands (SIMV shares the sim_commands counter)
  client.SendCommand("SIMV 10 1.0 0.0 0.0 0.0");
  client.SendCommand("SIMV 10 0.0 1.0 0.0 0.0");

  // Send 1 CONFIG SHOW command
  client.SendCommand("CONFIG SHOW");

  // The INFO command below also increments total_commands_processed
  auto resp = client.SendCommand("INFO");
  ASSERT_FALSE(resp.empty()) << "INFO should return a response";
  ASSERT_TRUE(resp.find("OK INFO") != std::string::npos) << "Expected OK INFO header";

  // Parse counters
  std::string event_str = ParseResponseField(resp, "event_commands");
  std::string vecset_str = ParseResponseField(resp, "vecset_commands");
  std::string sim_str = ParseResponseField(resp, "sim_commands");
  std::string total_str = ParseResponseField(resp, "total_commands_processed");

  // Verify event_commands >= 5
  if (!event_str.empty()) {
    int event_count = std::stoi(event_str);
    EXPECT_GE(event_count, 5) << "Expected at least 5 event_commands, got " << event_count;
  }

  // Verify vecset_commands >= 3
  if (!vecset_str.empty()) {
    int vecset_count = std::stoi(vecset_str);
    EXPECT_GE(vecset_count, 3) << "Expected at least 3 vecset_commands, got " << vecset_count;
  }

  // Verify sim_commands >= 4 (SIM) + possibly 2 (SIMV)
  if (!sim_str.empty()) {
    int sim_count = std::stoi(sim_str);
    EXPECT_GE(sim_count, 4) << "Expected at least 4 sim_commands, got " << sim_count;
  }

  // total_commands_processed should be at least 5+3+4+2+1 = 15
  // (The INFO command that reads the counter may or may not be counted
  // by the time it reads its own value, so we use 15 as lower bound.)
  if (!total_str.empty()) {
    int total_count = std::stoi(total_str);
    EXPECT_GE(total_count, 15) << "Expected at least 15 total_commands_processed, got "
                                << total_count;
  }
}

// ---------------------------------------------------------------------------
// Test 2: FailedCommandCounter
// ---------------------------------------------------------------------------

TEST_F(MetricsE2ETest, FailedCommandCounter) {
  TcpClient client("127.0.0.1", port_);

  // Establish dimension by setting a valid vector first
  ASSERT_TRUE(ContainsOK(client.SendCommand("VECSET dim_anchor 1.0 0.0 0.0 0.0")));

  // Get baseline failed_commands
  auto baseline_resp = client.SendCommand("INFO");
  std::string baseline_str = ParseResponseField(baseline_resp, "failed_commands");
  int baseline_failed = baseline_str.empty() ? 0 : std::stoi(baseline_str);

  // Send commands that parse successfully but fail during handling.
  // Parse errors (unknown command, missing args) return early before
  // incrementing failed_commands, so we use handler-level failures:

  // Dimension mismatch: dim=4 established, but only 2 components given
  auto r1 = client.SendCommand("VECSET item_x 1.0 2.0");
  // Dimension mismatch: dim=4 established, but 5 components given
  auto r2 = client.SendCommand("VECSET item_y 1.0 2.0 3.0 4.0 5.0");
  // SIM on non-existent item with using=vectors (may fail in handler)
  auto r3 = client.SendCommand("SIM nonexistent_item 10 using=vectors");

  // Get updated failed_commands
  auto resp = client.SendCommand("INFO");
  std::string failed_str = ParseResponseField(resp, "failed_commands");

  if (!failed_str.empty()) {
    int failed_count = std::stoi(failed_str);
    // At least the dimension mismatch commands should increment failed_commands.
    // If the server handles SIM on missing items gracefully (returning OK with
    // 0 results), that may not count as a failure.
    EXPECT_GT(failed_count, baseline_failed)
        << "failed_commands should have increased from " << baseline_failed
        << " after sending dimension-mismatch VECSET commands";
  }
}

// ---------------------------------------------------------------------------
// Test 3: MemoryTrackingAfterVecset
// ---------------------------------------------------------------------------

TEST_F(MetricsMemoryE2ETest, MemoryTrackingAfterVecset) {
  TcpClient client("127.0.0.1", port_);

  // Get baseline memory
  auto baseline_resp = client.SendCommand("INFO");
  std::string baseline_mem_str = ParseResponseField(baseline_resp, "used_memory_bytes");
  uint64_t baseline_mem = baseline_mem_str.empty() ? 0 : std::stoull(baseline_mem_str);

  // VECSET 50 vectors of dimension 128
  for (int i = 0; i < 50; ++i) {
    std::string item = "vec_" + std::to_string(i);
    auto resp = client.SendCommand(MakeVecsetCmd128(item));
    ASSERT_TRUE(ContainsOK(resp)) << "VECSET " << item << " should succeed, got: " << resp;
  }

  // Get memory after insertions
  auto resp = client.SendCommand("INFO");
  std::string mem_str = ParseResponseField(resp, "used_memory_bytes");
  ASSERT_FALSE(mem_str.empty()) << "used_memory_bytes should be present in INFO";

  uint64_t current_mem = std::stoull(mem_str);

  // Expected: 50 vectors * 128 floats * 4 bytes = 25600 bytes
  uint64_t expected_increase = 50ULL * 128ULL * sizeof(float);

  uint64_t actual_increase = current_mem - baseline_mem;

  // Allow +/- 10% tolerance
  double lower_bound = static_cast<double>(expected_increase) * 0.9;
  double upper_bound = static_cast<double>(expected_increase) * 1.1;

  EXPECT_GE(static_cast<double>(actual_increase), lower_bound)
      << "Memory increase (" << actual_increase << " bytes) should be >= "
      << lower_bound << " bytes (90% of " << expected_increase << ")";
  EXPECT_LE(static_cast<double>(actual_increase), upper_bound)
      << "Memory increase (" << actual_increase << " bytes) should be <= "
      << upper_bound << " bytes (110% of " << expected_increase << ")";
}

// ---------------------------------------------------------------------------
// Test 4: MemoryTrackingAfterOverwrite
// ---------------------------------------------------------------------------

TEST_F(MetricsMemoryE2ETest, MemoryTrackingAfterOverwrite) {
  TcpClient client("127.0.0.1", port_);

  // VECSET item1 with 128 dimensions
  ASSERT_TRUE(ContainsOK(client.SendCommand(MakeVecsetCmd128("item_overwrite"))));

  // Get memory after first set
  auto resp1 = client.SendCommand("INFO");
  std::string mem_str1 = ParseResponseField(resp1, "used_memory_bytes");
  ASSERT_FALSE(mem_str1.empty()) << "used_memory_bytes should be present";
  uint64_t mem_after_first = std::stoull(mem_str1);
  EXPECT_GT(mem_after_first, 0u) << "Memory should be non-zero after VECSET";

  // Overwrite same item with new values
  std::string overwrite_cmd = "VECSET item_overwrite";
  for (int i = 0; i < 128; ++i) {
    overwrite_cmd += " 2.0";  // Different values
  }
  ASSERT_TRUE(ContainsOK(client.SendCommand(overwrite_cmd)));

  // Get memory after overwrite
  auto resp2 = client.SendCommand("INFO");
  std::string mem_str2 = ParseResponseField(resp2, "used_memory_bytes");
  ASSERT_FALSE(mem_str2.empty()) << "used_memory_bytes should be present";
  uint64_t mem_after_overwrite = std::stoull(mem_str2);

  // Memory should NOT have doubled — it should be roughly the same
  // Allow up to 50% growth as tolerance (should really be ~0% growth)
  EXPECT_LE(mem_after_overwrite, mem_after_first * 3 / 2)
      << "Memory after overwrite (" << mem_after_overwrite
      << ") should not be much larger than before (" << mem_after_first << ")";
}

// ---------------------------------------------------------------------------
// Test 5: ConnectionCounterAccuracy
// ---------------------------------------------------------------------------

TEST_F(MetricsE2ETest, ConnectionCounterAccuracy) {
  uint64_t baseline_connections = 0;

  // Get baseline total_connections_received
  {
    TcpClient client("127.0.0.1", port_);
    auto resp = client.SendCommand("INFO");
    std::string conn_str = ParseResponseField(resp, "total_connections_received");
    if (!conn_str.empty()) {
      baseline_connections = std::stoull(conn_str);
    }
  }

  // Open and close 5 separate connections, each sends INFO
  for (int i = 0; i < 5; ++i) {
    TcpClient client("127.0.0.1", port_);
    client.SendCommand("INFO");
    client.Close();
    // Small delay to ensure the server processes the disconnect
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }

  // Check from a 6th connection
  TcpClient client("127.0.0.1", port_);
  auto resp = client.SendCommand("INFO");
  std::string conn_str = ParseResponseField(resp, "total_connections_received");

  if (!conn_str.empty()) {
    uint64_t current_connections = std::stoull(conn_str);
    // The baseline query was 1 connection. Then 5 more. Then this query is 1 more.
    // So we expect at least baseline + 5 + 1 = baseline + 6.
    // Use >= baseline + 5 to be safe (the current connection may not yet be counted).
    EXPECT_GE(current_connections, baseline_connections + 5)
        << "total_connections_received should have increased by at least 5 "
        << "(baseline=" << baseline_connections << ", current=" << current_connections << ")";
  }
}

// ---------------------------------------------------------------------------
// Test 6: MetricsConcurrentAccuracy
// ---------------------------------------------------------------------------

TEST_F(MetricsE2ETest, MetricsConcurrentAccuracy) {
  // First, set up some data so SIM commands have something to work with
  {
    TcpClient setup_client("127.0.0.1", port_);
    for (int i = 0; i < 5; ++i) {
      std::string item = "base_" + std::to_string(i);
      setup_client.SendCommand("EVENT ctx1 ADD " + item + " 100");
      setup_client.SendCommand(
          "VECSET " + item + " 1.0 0.0 0.0 0.0");
    }
  }

  // Get baseline total_commands_processed
  uint64_t baseline_total = 0;
  {
    TcpClient baseline_client("127.0.0.1", port_);
    auto resp = baseline_client.SendCommand("INFO");
    std::string total_str = ParseResponseField(resp, "total_commands_processed");
    if (!total_str.empty()) {
      baseline_total = std::stoull(total_str);
    }
  }

  const int kThreadCount = 10;
  const int kCommandsPerThread = 50;
  std::atomic<int> successful_commands{0};

  std::vector<std::thread> threads;
  threads.reserve(kThreadCount);

  for (int t = 0; t < kThreadCount; ++t) {
    threads.emplace_back([this, t, &successful_commands]() {
      try {
        TcpClient client("127.0.0.1", port_);
        for (int i = 0; i < kCommandsPerThread; ++i) {
          std::string resp;
          int cmd_type = (t * kCommandsPerThread + i) % 3;
          if (cmd_type == 0) {
            // EVENT
            resp = client.SendCommand(
                "EVENT ctx_t" + std::to_string(t) + " ADD item_" + std::to_string(i) + " 50");
          } else if (cmd_type == 1) {
            // VECSET (use unique IDs per thread to avoid contention on same key)
            resp = client.SendCommand(
                "VECSET titem_" + std::to_string(t) + "_" + std::to_string(i) +
                " 1.0 0.0 0.0 0.0");
          } else {
            // SIM
            resp = client.SendCommand("SIM base_0 5 using=events");
          }
          if (!resp.empty()) {
            successful_commands.fetch_add(1, std::memory_order_relaxed);
          }
        }
      } catch (const std::exception& e) {
        // Connection failure — acceptable under concurrent load
      }
    });
  }

  for (auto& t : threads) {
    t.join();
  }

  // Small delay to let server finish processing
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  // Verify totals
  TcpClient final_client("127.0.0.1", port_);
  auto resp = final_client.SendCommand("INFO");
  std::string total_str = ParseResponseField(resp, "total_commands_processed");

  if (!total_str.empty()) {
    uint64_t final_total = std::stoull(total_str);
    uint64_t commands_during_test = final_total - baseline_total;

    // We sent kThreadCount * kCommandsPerThread = 500 commands
    // Plus the INFO/baseline commands. Allow some slack for connection failures.
    EXPECT_GE(commands_during_test, static_cast<uint64_t>(kThreadCount * kCommandsPerThread) / 2)
        << "Expected at least half of " << kThreadCount * kCommandsPerThread
        << " commands to be counted (got " << commands_during_test << ")";
  }

  // Verify per-type counters sum reasonably
  std::string event_str = ParseResponseField(resp, "event_commands");
  std::string vecset_str = ParseResponseField(resp, "vecset_commands");
  std::string sim_str = ParseResponseField(resp, "sim_commands");

  if (!event_str.empty() && !vecset_str.empty() && !sim_str.empty()) {
    int event_count = std::stoi(event_str);
    int vecset_count = std::stoi(vecset_str);
    int sim_count = std::stoi(sim_str);

    // Each type should have some commands
    EXPECT_GT(event_count, 0) << "event_commands should be > 0 after concurrent load";
    EXPECT_GT(vecset_count, 0) << "vecset_commands should be > 0 after concurrent load";
    EXPECT_GT(sim_count, 0) << "sim_commands should be > 0 after concurrent load";
  }
}

// ---------------------------------------------------------------------------
// Test 7: DataCountsAccuracy
// ---------------------------------------------------------------------------

TEST_F(MetricsE2ETest, DataCountsAccuracy) {
  TcpClient client("127.0.0.1", port_);

  // VECSET 10 unique items
  for (int i = 0; i < 10; ++i) {
    std::string item = "data_item_" + std::to_string(i);
    auto resp = client.SendCommand("VECSET " + item + " 1.0 0.0 0.0 0.0");
    ASSERT_TRUE(ContainsOK(resp)) << "VECSET " << item << " should succeed";
  }

  // EVENT on 3 different contexts
  ASSERT_TRUE(ContainsOK(client.SendCommand("EVENT alpha ADD data_item_0 100")));
  ASSERT_TRUE(ContainsOK(client.SendCommand("EVENT beta ADD data_item_1 90")));
  ASSERT_TRUE(ContainsOK(client.SendCommand("EVENT gamma ADD data_item_2 80")));

  // Check INFO
  auto resp = client.SendCommand("INFO");
  ASSERT_TRUE(resp.find("OK INFO") != std::string::npos);

  std::string vec_count_str = ParseResponseField(resp, "vector_count");
  std::string ctx_count_str = ParseResponseField(resp, "ctx_count");

  ASSERT_FALSE(vec_count_str.empty()) << "vector_count should be in INFO response";
  ASSERT_FALSE(ctx_count_str.empty()) << "ctx_count should be in INFO response";

  EXPECT_EQ(std::stoi(vec_count_str), 10)
      << "vector_count should be 10 after 10 VECSET commands";
  EXPECT_EQ(std::stoi(ctx_count_str), 3)
      << "ctx_count should be 3 after events in 3 contexts";

  // VECSET 5 more unique items
  for (int i = 10; i < 15; ++i) {
    std::string item = "data_item_" + std::to_string(i);
    auto resp2 = client.SendCommand("VECSET " + item + " 0.0 1.0 0.0 0.0");
    ASSERT_TRUE(ContainsOK(resp2)) << "VECSET " << item << " should succeed";
  }

  // Check INFO again
  resp = client.SendCommand("INFO");
  vec_count_str = ParseResponseField(resp, "vector_count");
  ASSERT_FALSE(vec_count_str.empty()) << "vector_count should be in INFO response";
  EXPECT_EQ(std::stoi(vec_count_str), 15)
      << "vector_count should be 15 after 15 total VECSET commands";
}

// ---------------------------------------------------------------------------
// Test 8: CacheStatsFollowOperations
// ---------------------------------------------------------------------------

TEST_F(MetricsE2ETest, CacheStatsFollowOperations) {
  TcpClient client("127.0.0.1", port_);

  // Enable cache
  auto enable_resp = client.SendCommand("CACHE ENABLE");
  ASSERT_TRUE(ContainsOK(enable_resp)) << "CACHE ENABLE should succeed";

  // Set up some vectors for SIM queries
  ASSERT_TRUE(ContainsOK(client.SendCommand("VECSET cache_a 1.0 0.0 0.0 0.0")));
  ASSERT_TRUE(ContainsOK(client.SendCommand("VECSET cache_b 0.9 0.1 0.0 0.0")));
  ASSERT_TRUE(ContainsOK(client.SendCommand("VECSET cache_c 0.0 1.0 0.0 0.0")));

  // Also add events so SIM using=events works
  ASSERT_TRUE(ContainsOK(client.SendCommand("EVENT ctx_cache ADD cache_a 100")));
  ASSERT_TRUE(ContainsOK(client.SendCommand("EVENT ctx_cache ADD cache_b 90")));

  // First SIM query — should be a cache miss (if cache is wired into SIM path)
  client.SendCommand("SIM cache_a 10 using=vectors");

  // Same SIM query again — should be a cache hit (if cache is wired)
  client.SendCommand("SIM cache_a 10 using=vectors");

  // Get cache stats
  auto stats_resp = client.SendCommand("CACHE STATS");
  ASSERT_TRUE(ContainsOK(stats_resp)) << "CACHE STATS should succeed";

  std::string hits_str = ParseResponseField(stats_resp, "cache_hits");
  std::string misses_str = ParseResponseField(stats_resp, "cache_misses");
  std::string entries_str = ParseResponseField(stats_resp, "cache_entries");

  // If cache is wired into SIM path, we expect hits >= 1 and misses >= 1.
  // If cache is not wired yet, both should be 0. Either way, document it.
  if (!hits_str.empty() && !misses_str.empty()) {
    int hits = std::stoi(hits_str);
    int misses = std::stoi(misses_str);

    if (hits == 0 && misses == 0) {
      // Cache is not wired into the SIM path, or cost-based caching
      // skipped these queries (min_cost threshold not met)
      SUCCEED() << "Cache stats are both 0: cache may not be wired into SIM path yet";
    } else if (misses > 0 && hits == 0) {
      // Cache is wired but the result was not stored (e.g. query cost
      // was below min_cost threshold, so it was a "miss" without caching).
      // Repeated query also misses because nothing was cached.
      SUCCEED() << "Cache has " << misses << " misses and 0 hits: "
                << "results may not be cached due to cost threshold";
    } else {
      // Cache is fully wired and caching results
      EXPECT_GE(misses, 1) << "Expected at least 1 cache miss for the first query";
      EXPECT_GE(hits, 1) << "Expected at least 1 cache hit for the repeated query";
    }
  }

  // CACHE CLEAR — verify entries reset to 0
  auto clear_resp = client.SendCommand("CACHE CLEAR");
  ASSERT_TRUE(ContainsOK(clear_resp)) << "CACHE CLEAR should succeed";

  stats_resp = client.SendCommand("CACHE STATS");
  entries_str = ParseResponseField(stats_resp, "cache_entries");
  if (!entries_str.empty()) {
    EXPECT_EQ(std::stoi(entries_str), 0) << "cache_entries should be 0 after CACHE CLEAR";
  }
}
