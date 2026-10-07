/**
 * @file metrics_e2e_test.cpp
 * @brief End-to-end tests for metrics accuracy and memory tracking
 *
 * Verifies that server statistics counters (command counts, connection counts,
 * memory estimates, data counts, cache stats) are accurate after known
 * sequences of operations.
 */

#include <gtest/gtest.h>

#include <array>
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

  /// Read a documented counter; a missing field fails the test instead of
  /// skipping the check.
  static uint64_t Counter(const std::string& response, const std::string& field) {
    const std::string value = ParseResponseField(response, field);
    EXPECT_FALSE(value.empty()) << field << " missing from:\n" << response;
    return value.empty() ? 0 : std::stoull(value);
  }

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
  const std::string before = client.SendCommand("INFO");
  const uint64_t events = Counter(before, "event_commands");
  const uint64_t vecsets = Counter(before, "vecset_commands");
  const uint64_t sims = Counter(before, "sim_commands");
  const uint64_t total = Counter(before, "total_commands_processed");

  for (int i = 0; i < 5; ++i) {
    std::string cmd = "EVENT ctx1 ADD item_" + std::to_string(i) + " " + std::to_string((i + 1) * 10);
    ASSERT_TRUE(ContainsOK(client.SendCommand(cmd))) << "EVENT command " << i << " should succeed";
  }
  ASSERT_TRUE(ContainsOK(client.SendCommand("VECSET item_0 1.0 0.0 0.0 0.0")));
  ASSERT_TRUE(ContainsOK(client.SendCommand("VECSET item_1 0.9 0.1 0.0 0.0")));
  ASSERT_TRUE(ContainsOK(client.SendCommand("VECSET item_2 0.0 1.0 0.0 0.0")));
  for (int i = 0; i < 4; ++i) {
    ASSERT_TRUE(ContainsOK(client.SendCommand("SIM item_0 10 using=events")));
  }
  // SIMV shares the sim_commands counter.
  ASSERT_TRUE(ContainsOK(client.SendCommand("SIMV 10 1.0 0.0 0.0 0.0")));
  ASSERT_TRUE(ContainsOK(client.SendCommand("SIMV 10 0.0 1.0 0.0 0.0")));
  client.SendCommand("CONFIG SHOW");

  // INFO counts itself on entry, so the second INFO adds one to the total.
  const std::string after = client.SendCommand("INFO");
  ASSERT_NE(after.find("OK INFO"), std::string::npos);
  EXPECT_EQ(Counter(after, "event_commands"), events + 5);
  EXPECT_EQ(Counter(after, "vecset_commands"), vecsets + 3);
  EXPECT_EQ(Counter(after, "sim_commands"), sims + 6);
  EXPECT_EQ(Counter(after, "total_commands_processed"), total + 5 + 3 + 6 + 1 + 1);
}

// ---------------------------------------------------------------------------
// Test 2: FailedCommandCounter
// ---------------------------------------------------------------------------

TEST_F(MetricsE2ETest, FailedCommandCounter) {
  TcpClient client("127.0.0.1", port_);

  // Establish dimension by setting a valid vector first
  ASSERT_TRUE(ContainsOK(client.SendCommand("VECSET dim_anchor 1.0 0.0 0.0 0.0")));
  const uint64_t baseline_failed = Counter(client.SendCommand("INFO"), "failed_commands");

  // Two dimension mismatches and a SIM on an ID with no vector: three failures.
  EXPECT_EQ(client.SendCommand("VECSET item_x 1.0 2.0").rfind("ERROR", 0), 0U);
  EXPECT_EQ(client.SendCommand("VECSET item_y 1.0 2.0 3.0 4.0 5.0").rfind("ERROR", 0), 0U);
  EXPECT_EQ(client.SendCommand("SIM nonexistent_item 10 using=vectors").rfind("ERROR", 0), 0U);

  EXPECT_EQ(Counter(client.SendCommand("INFO"), "failed_commands"), baseline_failed + 3);
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
      << "Memory increase (" << actual_increase << " bytes) should be >= " << lower_bound << " bytes (90% of "
      << expected_increase << ")";
  EXPECT_LE(static_cast<double>(actual_increase), upper_bound)
      << "Memory increase (" << actual_increase << " bytes) should be <= " << upper_bound << " bytes (110% of "
      << expected_increase << ")";
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
      << "Memory after overwrite (" << mem_after_overwrite << ") should not be much larger than before ("
      << mem_after_first << ")";
}

// ---------------------------------------------------------------------------
// Test 5: ConnectionCounterAccuracy
// ---------------------------------------------------------------------------

TEST_F(MetricsE2ETest, ConnectionCounterAccuracy) {
  uint64_t baseline_connections = 0;
  {
    TcpClient client("127.0.0.1", port_);
    baseline_connections = Counter(client.SendCommand("INFO"), "total_connections_received");
  }

  // Open and close 5 separate connections, each sends INFO
  for (int i = 0; i < 5; ++i) {
    TcpClient client("127.0.0.1", port_);
    client.SendCommand("INFO");
    client.Close();
  }

  // A connection is counted when it is accepted, so this sixth one already is.
  TcpClient client("127.0.0.1", port_);
  EXPECT_EQ(Counter(client.SendCommand("INFO"), "total_connections_received"), baseline_connections + 6);
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
      ASSERT_TRUE(ContainsOK(setup_client.SendCommand("EVENT ctx1 ADD " + item + " 100")));
      ASSERT_TRUE(ContainsOK(setup_client.SendCommand("VECSET " + item + " 1.0 0.0 0.0 0.0")));
    }
  }

  std::string before;
  {
    TcpClient baseline_client("127.0.0.1", port_);
    before = baseline_client.SendCommand("INFO");
  }
  const uint64_t baseline_total = Counter(before, "total_commands_processed");
  const uint64_t baseline_events = Counter(before, "event_commands");
  const uint64_t baseline_vecsets = Counter(before, "vecset_commands");
  const uint64_t baseline_sims = Counter(before, "sim_commands");

  const int kThreadCount = 10;
  const int kCommandsPerThread = 50;
  std::array<std::atomic<uint64_t>, 3> answered{};

  std::vector<std::thread> threads;
  threads.reserve(kThreadCount);
  for (int t = 0; t < kThreadCount; ++t) {
    threads.emplace_back([this, t, &answered]() {
      TcpClient client("127.0.0.1", port_);
      for (int i = 0; i < kCommandsPerThread; ++i) {
        const int cmd_type = (t * kCommandsPerThread + i) % 3;
        std::string resp;
        if (cmd_type == 0) {
          resp = client.SendCommand("EVENT ctx_t" + std::to_string(t) + " ADD item_" + std::to_string(i) + " 50");
        } else if (cmd_type == 1) {
          resp = client.SendCommand("VECSET titem_" + std::to_string(t) + "_" + std::to_string(i) + " 1.0 0.0 0.0 0.0");
        } else {
          resp = client.SendCommand("SIM base_0 5 using=events");
        }
        if (!resp.empty()) {
          answered[cmd_type].fetch_add(1, std::memory_order_relaxed);
        }
      }
    });
  }
  for (auto& t : threads) {
    t.join();
  }

  // Every answered command was counted exactly once, under its own type.
  TcpClient final_client("127.0.0.1", port_);
  const std::string after = final_client.SendCommand("INFO");
  const uint64_t sent = static_cast<uint64_t>(kThreadCount) * kCommandsPerThread;
  EXPECT_EQ(answered[0] + answered[1] + answered[2], sent);
  EXPECT_EQ(Counter(after, "event_commands"), baseline_events + answered[0]);
  EXPECT_EQ(Counter(after, "vecset_commands"), baseline_vecsets + answered[1]);
  EXPECT_EQ(Counter(after, "sim_commands"), baseline_sims + answered[2]);
  EXPECT_EQ(Counter(after, "total_commands_processed"), baseline_total + sent + 1);
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

  EXPECT_EQ(std::stoi(vec_count_str), 10) << "vector_count should be 10 after 10 VECSET commands";
  EXPECT_EQ(std::stoi(ctx_count_str), 3) << "ctx_count should be 3 after events in 3 contexts";

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
  EXPECT_EQ(std::stoi(vec_count_str), 15) << "vector_count should be 15 after 15 total VECSET commands";
}

// ---------------------------------------------------------------------------
// Test 8: CacheStatsFollowOperations
// ---------------------------------------------------------------------------

TEST_F(MetricsE2ETest, CacheStatsFollowOperations) {
  TcpClient client("127.0.0.1", port_);

  ASSERT_TRUE(ContainsOK(client.SendCommand("CACHE ENABLE")));
  // Admit every result regardless of cost, so the repeat is a guaranteed hit.
  ASSERT_EQ(client.SendCommand("SET cache.min_query_cost_ms 0").rfind("+OK", 0), 0U);

  ASSERT_TRUE(ContainsOK(client.SendCommand("VECSET cache_a 1.0 0.0 0.0 0.0")));
  ASSERT_TRUE(ContainsOK(client.SendCommand("VECSET cache_b 0.9 0.1 0.0 0.0")));
  ASSERT_TRUE(ContainsOK(client.SendCommand("VECSET cache_c 0.0 1.0 0.0 0.0")));

  const std::string before = client.SendCommand("CACHE STATS");
  ASSERT_TRUE(ContainsOK(before));
  const uint64_t hits = Counter(before, "cache_hits");
  const uint64_t misses = Counter(before, "cache_misses");

  ASSERT_TRUE(ContainsOK(client.SendCommand("SIM cache_a 10 using=vectors")));  // miss, stored
  ASSERT_TRUE(ContainsOK(client.SendCommand("SIM cache_a 10 using=vectors")));  // hit

  const std::string after = client.SendCommand("CACHE STATS");
  ASSERT_TRUE(ContainsOK(after));
  EXPECT_EQ(Counter(after, "cache_misses"), misses + 1);
  EXPECT_EQ(Counter(after, "cache_hits"), hits + 1);
  EXPECT_GE(Counter(after, "cache_entries"), 1U);

  ASSERT_TRUE(ContainsOK(client.SendCommand("CACHE CLEAR")));
  EXPECT_EQ(Counter(client.SendCommand("CACHE STATS"), "cache_entries"), 0U);
}
