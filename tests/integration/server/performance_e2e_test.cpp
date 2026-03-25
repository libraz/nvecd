/**
 * @file performance_e2e_test.cpp
 * @brief Performance and benchmark tests for nvecd server
 *
 * Reference: ../mygram-db/tests/integration/server/stress_test.cpp
 *
 * Tests measure throughput, latency, and scalability of core operations.
 * All tests are DISABLED by default (CI-safe). Run with:
 *   --gtest_also_run_disabled_tests --gtest_filter="PerformanceE2ETest.*"
 *
 * Test categories:
 * - Ingestion throughput (EVENT, VECSET)
 * - Query latency (SIM, SIMV) at various scales
 * - Scalability (search time vs. data size)
 * - Concurrent throughput (mixed read/write)
 * - Snapshot I/O performance (DUMP SAVE/LOAD)
 * - Memory efficiency
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <thread>
#include <vector>

#include "../test_server_fixture.h"
#include "../test_tcp_client.h"

namespace {

/**
 * @brief Measure execution time and return duration in milliseconds
 */
template <typename Func>
double MeasureMs(Func&& func) {
  auto start = std::chrono::high_resolution_clock::now();
  func();
  auto end = std::chrono::high_resolution_clock::now();
  return std::chrono::duration<double, std::milli>(end - start).count();
}

/**
 * @brief Compute percentile from sorted durations
 */
double Percentile(std::vector<double>& sorted_values, double p) {
  if (sorted_values.empty()) {
    return 0.0;
  }
  size_t idx = static_cast<size_t>(p / 100.0 * static_cast<double>(sorted_values.size() - 1));
  return sorted_values[idx];
}

/**
 * @brief Print performance report header
 */
void PrintHeader(const std::string& test_name) {
  std::cout << "\n========================================\n";
  std::cout << "  " << test_name << "\n";
  std::cout << "========================================\n";
}

/**
 * @brief Print throughput metrics
 */
void PrintThroughput(const std::string& label, int ops, double duration_ms) {
  double ops_per_sec = (duration_ms > 0) ? (static_cast<double>(ops) / duration_ms * 1000.0) : 0.0;
  std::cout << std::fixed << std::setprecision(1);
  std::cout << "  " << label << ": " << ops << " ops in " << duration_ms << " ms (" << ops_per_sec << " ops/sec)\n";
}

/**
 * @brief Print latency percentiles
 */
void PrintLatency(const std::string& label, std::vector<double>& latencies_ms) {
  if (latencies_ms.empty()) {
    return;
  }
  std::sort(latencies_ms.begin(), latencies_ms.end());
  double avg =
      std::accumulate(latencies_ms.begin(), latencies_ms.end(), 0.0) / static_cast<double>(latencies_ms.size());
  std::cout << std::fixed << std::setprecision(3);
  std::cout << "  " << label << " latency:\n";
  std::cout << "    avg=" << avg << " ms, p50=" << Percentile(latencies_ms, 50)
            << " ms, p95=" << Percentile(latencies_ms, 95) << " ms, p99=" << Percentile(latencies_ms, 99)
            << " ms, max=" << latencies_ms.back() << " ms\n";
}

}  // namespace

// ============================================================================
// Fixture: 3-dimensional vectors for fast benchmarks
// ============================================================================

class PerformanceE2ETest : public NvecdTestFixture {
 protected:
  void SetUp() override { SetUpServer(3); }
  void TearDown() override { TearDownServer(); }

  /**
   * @brief Bulk-add vectors via TCP
   */
  void AddVectors(TcpClient& client, int count, int offset = 0) {
    for (int i = 0; i < count; ++i) {
      int idx = offset + i;
      std::string id = "v_" + std::to_string(idx);
      float v1 = static_cast<float>(idx % 100) / 100.0F;
      float v2 = static_cast<float>(idx % 50) / 50.0F;
      float v3 = 1.0F - v1;
      client.SendCommand("VECSET " + id + " " + std::to_string(v1) + " " + std::to_string(v2) + " " +
                         std::to_string(v3));
    }
  }

  /**
   * @brief Bulk-add events via TCP
   */
  void AddEvents(TcpClient& client, int count, int offset = 0) {
    for (int i = 0; i < count; ++i) {
      int idx = offset + i;
      std::string ctx = "ctx_" + std::to_string(idx / 10);
      std::string id = "v_" + std::to_string(idx);
      client.SendCommand("EVENT " + ctx + " ADD " + id + " " + std::to_string(100 - (idx % 100)));
    }
  }
};

// ============================================================================
// Test 1: EVENT ingestion throughput
// ============================================================================

TEST_F(PerformanceE2ETest, DISABLED_EventIngestionThroughput) {
  PrintHeader("EVENT Ingestion Throughput");
  TcpClient client("127.0.0.1", port_);

  const int kCount = 50000;
  double duration_ms = MeasureMs([&]() { AddEvents(client, kCount); });

  PrintThroughput("EVENT ADD", kCount, duration_ms);

  // Verify data was ingested
  auto info = client.SendCommand("INFO");
  std::string event_count = ParseResponseField(info, "event_count");
  std::cout << "  Events stored: " << event_count << "\n";
}

// ============================================================================
// Test 2: VECSET ingestion throughput
// ============================================================================

TEST_F(PerformanceE2ETest, DISABLED_VecsetIngestionThroughput) {
  PrintHeader("VECSET Ingestion Throughput");
  TcpClient client("127.0.0.1", port_);

  const int kCount = 50000;
  double duration_ms = MeasureMs([&]() { AddVectors(client, kCount); });

  PrintThroughput("VECSET", kCount, duration_ms);

  // Verify
  auto info = client.SendCommand("INFO");
  std::string vec_count = ParseResponseField(info, "vector_count");
  std::string mem = ParseResponseField(info, "used_memory_bytes");
  std::cout << "  Vectors stored: " << vec_count << "\n";
  std::cout << "  Memory used: " << mem << " bytes\n";
}

// ============================================================================
// Test 3: SIM query latency at various scales
// ============================================================================

TEST_F(PerformanceE2ETest, DISABLED_SimQueryLatencyByScale) {
  PrintHeader("SIM Query Latency by Data Scale");
  TcpClient client("127.0.0.1", port_);

  std::vector<int> scales = {100, 500, 1000, 5000, 10000};
  const int kQueries = 100;

  int loaded = 0;
  for (int scale : scales) {
    // Incrementally add vectors to reach target scale
    int to_add = scale - loaded;
    AddVectors(client, to_add, loaded);
    AddEvents(client, to_add, loaded);
    loaded = scale;

    // Measure latencies
    std::vector<double> latencies;
    latencies.reserve(kQueries);

    for (int q = 0; q < kQueries; ++q) {
      std::string id = "v_" + std::to_string(q % scale);
      double lat = MeasureMs([&]() { client.SendCommand("SIM " + id + " 10 using=vectors"); });
      latencies.push_back(lat);
    }

    std::string label = "scale=" + std::to_string(scale);
    PrintLatency(label, latencies);
  }

  // Verify search still works at max scale
  auto resp = client.SendCommand("SIM v_0 10 using=vectors");
  EXPECT_TRUE(ContainsOK(resp));
}

// ============================================================================
// Test 4: SIMV query latency
// ============================================================================

TEST_F(PerformanceE2ETest, DISABLED_SimvQueryLatency) {
  PrintHeader("SIMV Query Latency");
  TcpClient client("127.0.0.1", port_);

  const int kVectors = 10000;
  AddVectors(client, kVectors);

  const int kQueries = 200;
  std::vector<double> latencies;
  latencies.reserve(kQueries);

  for (int q = 0; q < kQueries; ++q) {
    float v1 = static_cast<float>(q % 100) / 100.0F;
    float v2 = static_cast<float>(q % 50) / 50.0F;
    float v3 = 1.0F - v1;
    std::string cmd = "SIMV 10 " + std::to_string(v1) + " " + std::to_string(v2) + " " + std::to_string(v3);
    double lat = MeasureMs([&]() { client.SendCommand(cmd); });
    latencies.push_back(lat);
  }

  PrintLatency("SIMV (10K vectors)", latencies);
}

// ============================================================================
// Test 5: Search performance degradation (scalability)
// ============================================================================

TEST_F(PerformanceE2ETest, DISABLED_SearchScalability) {
  PrintHeader("Search Scalability (SIM using=vectors)");
  TcpClient client("127.0.0.1", port_);

  std::vector<int> scales = {1000, 5000, 10000, 50000};
  const int kQueries = 50;
  std::vector<std::pair<int, double>> results;

  int loaded = 0;
  for (int scale : scales) {
    int to_add = scale - loaded;
    AddVectors(client, to_add, loaded);
    loaded = scale;

    // Warm up
    client.SendCommand("SIM v_0 10 using=vectors");

    // Measure
    double total_ms = MeasureMs([&]() {
      for (int q = 0; q < kQueries; ++q) {
        client.SendCommand("SIM v_0 10 using=vectors");
      }
    });

    double avg_ms = total_ms / kQueries;
    results.emplace_back(scale, avg_ms);
    std::cout << "  " << scale << " vectors: avg " << std::fixed << std::setprecision(3) << avg_ms << " ms/query\n";
  }

  // Check degradation: O(n) is expected for brute-force search
  // time should scale roughly linearly with data size
  if (results.size() >= 2) {
    double first_time = results.front().second;
    double last_time = results.back().second;
    auto first_scale = static_cast<double>(results.front().first);
    auto last_scale = static_cast<double>(results.back().first);

    double scale_ratio = last_scale / first_scale;
    double time_ratio = (first_time > 0) ? (last_time / first_time) : 0;

    std::cout << "\n  Scale ratio: " << std::setprecision(1) << scale_ratio << "x\n";
    std::cout << "  Time ratio:  " << time_ratio << "x\n";
    std::cout << "  Complexity:  ~O(n^" << std::setprecision(2) << (std::log(time_ratio) / std::log(scale_ratio))
              << ")\n";

    // Should not be worse than O(n^2)
    EXPECT_LT(time_ratio, scale_ratio * scale_ratio) << "Performance worse than O(n^2)";
  }
}

// ============================================================================
// Test 6: Concurrent query throughput
// ============================================================================

TEST_F(PerformanceE2ETest, DISABLED_ConcurrentQueryThroughput) {
  PrintHeader("Concurrent Query Throughput");
  TcpClient setup("127.0.0.1", port_);

  const int kVectors = 5000;
  AddVectors(setup, kVectors);
  AddEvents(setup, kVectors);

  // Test with different thread counts
  std::vector<int> thread_counts = {1, 2, 4, 8};
  const int kQueriesPerThread = 200;

  for (int num_threads : thread_counts) {
    std::atomic<int> total_success{0};
    std::vector<std::thread> threads;

    double duration_ms = MeasureMs([&]() {
      for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([this, &total_success]() {
          TcpClient client("127.0.0.1", port_);
          for (int q = 0; q < kQueriesPerThread; ++q) {
            auto resp = client.SendCommand("SIM v_0 10 using=vectors");
            if (ContainsOK(resp)) {
              total_success.fetch_add(1, std::memory_order_relaxed);
            }
          }
        });
      }
      for (auto& t : threads) {
        t.join();
      }
    });

    int total_ops = num_threads * kQueriesPerThread;
    PrintThroughput(std::to_string(num_threads) + " threads", total_ops, duration_ms);
    EXPECT_EQ(total_success.load(), total_ops);
  }
}

// ============================================================================
// Test 7: Mixed workload throughput (read-heavy, write-heavy, balanced)
// ============================================================================

TEST_F(PerformanceE2ETest, DISABLED_MixedWorkloadThroughput) {
  PrintHeader("Mixed Workload Throughput");
  TcpClient setup("127.0.0.1", port_);

  const int kVectors = 5000;
  AddVectors(setup, kVectors);
  AddEvents(setup, kVectors);

  struct Workload {
    std::string name;
    int read_pct;   // SIM queries
    int write_pct;  // EVENT + VECSET
  };

  std::vector<Workload> workloads = {
      {"read-heavy (90/10)", 90, 10},
      {"balanced (50/50)", 50, 50},
      {"write-heavy (10/90)", 10, 90},
  };

  const int kThreads = 4;
  const int kOpsPerThread = 500;

  for (const auto& wl : workloads) {
    std::atomic<int> success{0};
    std::atomic<int> write_idx{static_cast<int>(kVectors)};
    std::vector<std::thread> threads;

    double duration_ms = MeasureMs([&]() {
      for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t]() {
          TcpClient client("127.0.0.1", port_);
          for (int op = 0; op < kOpsPerThread; ++op) {
            bool is_read = (op % 100) < wl.read_pct;
            std::string resp;

            if (is_read) {
              std::string id = "v_" + std::to_string(op % kVectors);
              resp = client.SendCommand("SIM " + id + " 10 using=vectors");
            } else {
              int idx = write_idx.fetch_add(1, std::memory_order_relaxed);
              std::string id = "w_" + std::to_string(idx);
              float v = static_cast<float>(idx % 100) / 100.0F;
              client.SendCommand("EVENT wctx_" + std::to_string(t) + " ADD " + id + " 100");
              resp = client.SendCommand("VECSET " + id + " " + std::to_string(v) + " 0.5 0.5");
            }

            if (!resp.empty()) {
              success.fetch_add(1, std::memory_order_relaxed);
            }
          }
        });
      }
      for (auto& t : threads) {
        t.join();
      }
    });

    int total_ops = kThreads * kOpsPerThread;
    PrintThroughput(wl.name, total_ops, duration_ms);
  }
}

// ============================================================================
// Test 8: Snapshot I/O performance
// ============================================================================

TEST_F(PerformanceE2ETest, DISABLED_SnapshotIOPerformance) {
  PrintHeader("Snapshot I/O Performance");
  TcpClient client("127.0.0.1", port_);

  std::vector<int> scales = {1000, 5000, 10000};

  int loaded = 0;
  for (int scale : scales) {
    int to_add = scale - loaded;
    AddVectors(client, to_add, loaded);
    AddEvents(client, to_add, loaded);
    loaded = scale;

    std::string filename = "perf_" + std::to_string(scale) + ".dmp";

    // Measure SAVE
    double save_ms = MeasureMs([&]() { client.SendCommand("DUMP SAVE " + filename); });

    // Measure VERIFY
    double verify_ms = MeasureMs([&]() { client.SendCommand("DUMP VERIFY " + filename); });

    // Measure INFO
    double info_ms = MeasureMs([&]() { client.SendCommand("DUMP INFO " + filename); });

    std::cout << "  " << scale << " items:\n";
    std::cout << "    SAVE:   " << std::fixed << std::setprecision(1) << save_ms << " ms\n";
    std::cout << "    VERIFY: " << verify_ms << " ms\n";
    std::cout << "    INFO:   " << info_ms << " ms\n";
  }
}

// ============================================================================
// Test 9: Memory efficiency per vector
// ============================================================================

TEST_F(PerformanceE2ETest, DISABLED_MemoryEfficiency) {
  PrintHeader("Memory Efficiency");
  TcpClient client("127.0.0.1", port_);

  // Baseline memory
  auto info0 = client.SendCommand("INFO");
  std::string mem0_str = ParseResponseField(info0, "used_memory_bytes");
  int64_t mem_baseline = mem0_str.empty() ? 0 : std::stoll(mem0_str);

  std::vector<int> checkpoints = {1000, 5000, 10000, 50000};
  int loaded = 0;

  for (int cp : checkpoints) {
    int to_add = cp - loaded;
    AddVectors(client, to_add, loaded);
    loaded = cp;

    auto info = client.SendCommand("INFO");
    std::string mem_str = ParseResponseField(info, "used_memory_bytes");
    int64_t mem = mem_str.empty() ? 0 : std::stoll(mem_str);
    int64_t mem_delta = mem - mem_baseline;

    double bytes_per_vector = (loaded > 0) ? static_cast<double>(mem_delta) / loaded : 0;
    // 3-dim float vector = 12 bytes raw data. Overhead includes ID string, map entry, etc.
    std::cout << "  " << cp << " vectors: " << mem_delta << " bytes total, " << std::fixed << std::setprecision(1)
              << bytes_per_vector << " bytes/vector (raw: 12 bytes)\n";
  }
}

// ============================================================================
// Test 10: top_k impact on query latency
// ============================================================================

TEST_F(PerformanceE2ETest, DISABLED_TopKImpactOnLatency) {
  PrintHeader("top_k Impact on Query Latency");
  TcpClient client("127.0.0.1", port_);

  const int kVectors = 10000;
  AddVectors(client, kVectors);

  std::vector<int> top_k_values = {1, 5, 10, 50, 100};
  const int kQueries = 100;

  for (int k : top_k_values) {
    std::vector<double> latencies;
    latencies.reserve(kQueries);

    for (int q = 0; q < kQueries; ++q) {
      double lat = MeasureMs([&]() { client.SendCommand("SIM v_0 " + std::to_string(k) + " using=vectors"); });
      latencies.push_back(lat);
    }

    PrintLatency("top_k=" + std::to_string(k), latencies);
  }
}

// ============================================================================
// Test 11: Fusion vs. single-mode query latency comparison
// ============================================================================

TEST_F(PerformanceE2ETest, DISABLED_FusionVsSingleModeLatency) {
  PrintHeader("Query Mode Latency Comparison");
  TcpClient client("127.0.0.1", port_);

  const int kVectors = 5000;
  AddVectors(client, kVectors);
  AddEvents(client, kVectors);

  std::vector<std::string> modes = {"events", "vectors", "fusion"};
  const int kQueries = 200;

  for (const auto& mode : modes) {
    std::vector<double> latencies;
    latencies.reserve(kQueries);

    for (int q = 0; q < kQueries; ++q) {
      std::string id = "v_" + std::to_string(q % kVectors);
      double lat = MeasureMs([&]() { client.SendCommand("SIM " + id + " 10 using=" + mode); });
      latencies.push_back(lat);
    }

    PrintLatency("using=" + mode, latencies);
  }
}
