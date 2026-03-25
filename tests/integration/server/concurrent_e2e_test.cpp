/**
 * @file concurrent_e2e_test.cpp
 * @brief Concurrency and thread-safety integration tests for nvecd server
 *
 * Tests thread safety through the full TCP server including:
 * - Concurrent reads and writes
 * - Mixed read/write workloads
 * - Rapid reconnection handling
 * - Memory stability under load
 * - Cache behavior under concurrent access
 * - Dump operations during concurrent writes
 * - Large-scale vector operations (disabled by default)
 * - High concurrency stress tests (disabled by default)
 */

#include <gtest/gtest.h>

#include <atomic>
#include <iostream>
#include <thread>
#include <vector>

#include "../test_server_fixture.h"
#include "../test_tcp_client.h"

/**
 * @brief Fixture for concurrent end-to-end tests
 *
 * Uses 3-dimensional vectors for all tests.
 */
class ConcurrentE2ETest : public NvecdTestFixture {
 protected:
  void SetUp() override { SetUpServer(3); }
  void TearDown() override { TearDownServer(); }
};

TEST_F(ConcurrentE2ETest, ConcurrentReads) {
  TcpClient setup_client("127.0.0.1", port_);
  PopulateBasicData(setup_client);

  std::atomic<int> success_count{0};
  std::vector<std::thread> threads;

  for (int i = 0; i < 10; ++i) {
    threads.emplace_back([this, &success_count]() {
      TcpClient client("127.0.0.1", port_);
      for (int j = 0; j < 50; ++j) {
        std::string response = client.SendCommand("SIM item1 10 using=vectors");
        if (ContainsOK(response)) {
          success_count.fetch_add(1, std::memory_order_relaxed);
        }
      }
    });
  }

  for (auto& t : threads) {
    t.join();
  }

  EXPECT_EQ(success_count.load(), 500);
}

TEST_F(ConcurrentE2ETest, ConcurrentWrites) {
  const int kThreads = 10;
  const int kOpsPerThread = 50;
  std::vector<std::thread> threads;

  for (int i = 0; i < kThreads; ++i) {
    threads.emplace_back([this, i]() {
      TcpClient client("127.0.0.1", port_);
      for (int j = 0; j < kOpsPerThread; ++j) {
        std::string item = "item_" + std::to_string(i) + "_" + std::to_string(j);
        std::string ctx = "ctx_" + std::to_string(i);
        client.SendCommand("EVENT " + ctx + " ADD " + item + " 100");

        float v1 = static_cast<float>(i) / static_cast<float>(kThreads);
        float v2 = static_cast<float>(j) / static_cast<float>(kOpsPerThread);
        float v3 = 1.0F - v1;
        client.SendCommand("VECSET " + item + " " + std::to_string(v1) + " " + std::to_string(v2) + " " +
                           std::to_string(v3));
      }
    });
  }

  for (auto& t : threads) {
    t.join();
  }

  TcpClient check_client("127.0.0.1", port_);
  std::string info_response = check_client.SendCommand("INFO");
  std::string vector_count_str = ParseResponseField(info_response, "vector_count");
  ASSERT_FALSE(vector_count_str.empty()) << "Failed to parse vector_count from INFO response";

  int vector_count = std::stoi(vector_count_str);
  int expected = kThreads * kOpsPerThread;
  EXPECT_GE(vector_count, expected) << "Expected at least " << expected << " vectors, got " << vector_count;
}

TEST_F(ConcurrentE2ETest, MixedReadWrite) {
  TcpClient setup_client("127.0.0.1", port_);
  PopulateBasicData(setup_client);

  std::atomic<int> reader_valid_count{0};
  std::vector<std::thread> threads;

  // 5 reader threads
  for (int i = 0; i < 5; ++i) {
    threads.emplace_back([this, &reader_valid_count]() {
      TcpClient client("127.0.0.1", port_);
      for (int j = 0; j < 50; ++j) {
        std::string response = client.SendCommand("SIM item1 10 using=vectors");
        if (!response.empty()) {
          reader_valid_count.fetch_add(1, std::memory_order_relaxed);
        }
      }
    });
  }

  // 5 writer threads
  for (int i = 0; i < 5; ++i) {
    threads.emplace_back([this, i]() {
      TcpClient client("127.0.0.1", port_);
      for (int j = 0; j < 50; ++j) {
        std::string item = "new_item_" + std::to_string(i) + "_" + std::to_string(j);
        client.SendCommand("EVENT ctx_w" + std::to_string(i) + " ADD " + item + " 100");

        float v1 = static_cast<float>(i) * 0.1F;
        float v2 = static_cast<float>(j) * 0.02F;
        client.SendCommand("VECSET " + item + " " + std::to_string(v1) + " " + std::to_string(v2) + " 0.5");
      }
    });
  }

  for (auto& t : threads) {
    t.join();
  }

  // All readers should have received valid responses (no crashes)
  EXPECT_EQ(reader_valid_count.load(), 250);
}

TEST_F(ConcurrentE2ETest, RapidReconnect) {
  for (int i = 0; i < 50; ++i) {
    TcpClient client("127.0.0.1", port_);
    client.SendCommand("INFO");
    client.Close();
  }

  // After rapid reconnections, server should still be responsive
  TcpClient final_client("127.0.0.1", port_);
  std::string response = final_client.SendCommand("INFO");
  EXPECT_TRUE(ContainsOK(response)) << "Server unresponsive after rapid reconnections";
}

TEST_F(ConcurrentE2ETest, MemoryStabilityUnderLoad) {
  TcpClient client("127.0.0.1", port_);
  const int kCycles = 5;
  const int kEventsPerCycle = 100;
  int64_t first_cycle_memory = 0;
  int64_t last_cycle_memory = 0;

  for (int cycle = 0; cycle < kCycles; ++cycle) {
    std::string ctx = "mem_ctx_" + std::to_string(cycle);
    for (int j = 0; j < kEventsPerCycle; ++j) {
      std::string item = "mem_item_" + std::to_string(cycle) + "_" + std::to_string(j);
      client.SendCommand("EVENT " + ctx + " ADD " + item + " 100");
      // Add vectors so used_memory_bytes reflects actual data
      float v1 = static_cast<float>(cycle) / 5.0F;
      float v2 = static_cast<float>(j) / 100.0F;
      float v3 = 1.0F - v1;
      client.SendCommand("VECSET " + item + " " + std::to_string(v1) + " " + std::to_string(v2) + " " +
                         std::to_string(v3));
    }

    std::string info = client.SendCommand("INFO");
    std::string memory_str = ParseResponseField(info, "used_memory_bytes");
    ASSERT_FALSE(memory_str.empty()) << "Failed to parse used_memory_bytes from INFO at cycle " << cycle;

    int64_t memory = std::stoll(memory_str);
    if (cycle == 0) {
      first_cycle_memory = memory;
    }
    if (cycle == kCycles - 1) {
      last_cycle_memory = memory;
    }
  }

  // Memory grows linearly with data added (100 vectors per cycle).
  // After 5 cycles of 100 vectors each, memory should be ~5x cycle 0.
  // Verify no unbounded growth: last cycle should be <= kCycles * first + margin.
  ASSERT_GT(first_cycle_memory, 0) << "First cycle memory should be positive";
  int64_t expected_max = first_cycle_memory * (kCycles + 1);
  EXPECT_LE(last_cycle_memory, expected_max) << "Memory grew beyond expected linear bound: first=" << first_cycle_memory
                                             << " last=" << last_cycle_memory << " expected_max=" << expected_max;
}

TEST_F(ConcurrentE2ETest, CacheUnderConcurrentAccess) {
  TcpClient setup_client("127.0.0.1", port_);
  PopulateBasicData(setup_client);

  // Disable min query cost threshold so small queries get cached
  setup_client.SendCommand("SET cache.min_query_cost_ms 0");

  std::atomic<int> sim_success{0};
  std::vector<std::thread> threads;

  for (int i = 0; i < 10; ++i) {
    threads.emplace_back([this, &sim_success]() {
      TcpClient client("127.0.0.1", port_);
      for (int j = 0; j < 50; ++j) {
        std::string response = client.SendCommand("SIM item1 10 using=vectors");
        if (ContainsOK(response)) {
          sim_success.fetch_add(1, std::memory_order_relaxed);
        }
      }
    });
  }

  for (auto& t : threads) {
    t.join();
  }

  // Verify all SIM queries succeeded under concurrent cache access
  EXPECT_EQ(sim_success.load(), 500);

  // Verify CACHE STATS responds correctly after concurrent load
  TcpClient stats_client("127.0.0.1", port_);
  std::string stats = stats_client.SendCommand("CACHE STATS");
  EXPECT_TRUE(ContainsOK(stats)) << "CACHE STATS should return OK";

  std::string cache_hits_str = ParseResponseField(stats, "cache_hits");
  ASSERT_FALSE(cache_hits_str.empty()) << "Failed to parse cache_hits from CACHE STATS";

  int cache_hits = std::stoi(cache_hits_str);
  EXPECT_GE(cache_hits, 0) << "cache_hits should be a valid non-negative value";
}

TEST_F(ConcurrentE2ETest, DumpSaveDuringConcurrentWrites) {
  TcpClient setup_client("127.0.0.1", port_);
  PopulateBasicData(setup_client);

  std::atomic<bool> dump_ok{false};
  std::vector<std::thread> threads;

  // 5 writer threads
  for (int i = 0; i < 5; ++i) {
    threads.emplace_back([this, i]() {
      TcpClient client("127.0.0.1", port_);
      for (int j = 0; j < 50; ++j) {
        std::string item = "dump_item_" + std::to_string(i) + "_" + std::to_string(j);
        client.SendCommand("EVENT dump_ctx_" + std::to_string(i) + " ADD " + item + " 100");
      }
    });
  }

  // 1 thread that performs DUMP SAVE
  threads.emplace_back([this, &dump_ok]() {
    TcpClient client("127.0.0.1", port_);
    std::string response = client.SendCommand("DUMP SAVE concurrent_test.dmp");
    if (ContainsOK(response)) {
      dump_ok.store(true, std::memory_order_relaxed);
    }
  });

  for (auto& t : threads) {
    t.join();
  }

  EXPECT_TRUE(dump_ok.load()) << "DUMP SAVE should return OK during concurrent writes";
}

TEST_F(ConcurrentE2ETest, DISABLED_LargeScale10KVectors) {
  TcpClient client("127.0.0.1", port_);
  const int kVectorCount = 10000;

  // Add 10000 3-dimensional vectors
  for (int i = 0; i < kVectorCount; ++i) {
    std::string item = "large_item_" + std::to_string(i);
    float v1 = static_cast<float>(i % 100) / 100.0F;
    float v2 = static_cast<float>(i % 50) / 50.0F;
    float v3 = static_cast<float>(i % 25) / 25.0F;
    client.SendCommand("EVENT large_ctx ADD " + item + " 100");
    client.SendCommand("VECSET " + item + " " + std::to_string(v1) + " " + std::to_string(v2) + " " +
                       std::to_string(v3));
  }

  // Measure SIM throughput: 100 queries
  const int kQueryCount = 100;
  std::atomic<int> query_successes{0};

  auto start = std::chrono::high_resolution_clock::now();

  for (int i = 0; i < kQueryCount; ++i) {
    std::string response = client.SendCommand("SIM large_item_0 10 using=vectors");
    if (ContainsOK(response)) {
      query_successes.fetch_add(1, std::memory_order_relaxed);
    }
  }

  auto end = std::chrono::high_resolution_clock::now();
  auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

  double ops_per_sec =
      (duration_ms > 0) ? (static_cast<double>(kQueryCount) / static_cast<double>(duration_ms) * 1000.0) : 0.0;

  std::cout << "[LargeScale10KVectors] " << kVectorCount << " vectors loaded" << std::endl;
  std::cout << "[LargeScale10KVectors] " << kQueryCount << " SIM queries in " << duration_ms << " ms" << std::endl;
  std::cout << "[LargeScale10KVectors] Throughput: " << ops_per_sec << " ops/sec" << std::endl;

  EXPECT_EQ(query_successes.load(), kQueryCount);
}

TEST_F(ConcurrentE2ETest, DISABLED_HighConcurrency50Threads) {
  TcpClient setup_client("127.0.0.1", port_);
  PopulateBasicData(setup_client);

  const int kThreads = 50;
  const int kOpsPerThread = 100;
  std::atomic<int> success_count{0};
  std::vector<std::thread> threads;

  for (int i = 0; i < kThreads; ++i) {
    threads.emplace_back([this, i, &success_count]() {
      try {
        TcpClient client("127.0.0.1", port_);
        for (int j = 0; j < kOpsPerThread; ++j) {
          std::string response;
          int op = j % 3;
          if (op == 0) {
            response = client.SendCommand("SIM item1 10 using=vectors");
          } else if (op == 1) {
            std::string item = "hc_item_" + std::to_string(i) + "_" + std::to_string(j);
            response = client.SendCommand("EVENT hc_ctx_" + std::to_string(i) + " ADD " + item + " 100");
          } else {
            std::string item = "hc_item_" + std::to_string(i) + "_" + std::to_string(j);
            float v1 = static_cast<float>(i) / static_cast<float>(kThreads);
            float v2 = static_cast<float>(j) / static_cast<float>(kOpsPerThread);
            response =
                client.SendCommand("VECSET " + item + " " + std::to_string(v1) + " " + std::to_string(v2) + " 0.5");
          }
          if (ContainsOK(response)) {
            success_count.fetch_add(1, std::memory_order_relaxed);
          }
        }
      } catch (const std::runtime_error& e) {
        // Connection failures are expected under high concurrency
        std::cout << "[HighConcurrency] Thread " << i << " connection error: " << e.what() << std::endl;
      }
    });
  }

  auto start = std::chrono::high_resolution_clock::now();

  for (auto& t : threads) {
    t.join();
  }

  auto end = std::chrono::high_resolution_clock::now();
  auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

  int total_ops = kThreads * kOpsPerThread;
  int successes = success_count.load();
  double success_rate = static_cast<double>(successes) / static_cast<double>(total_ops) * 100.0;

  std::cout << "[HighConcurrency50Threads] " << kThreads << " threads x " << kOpsPerThread << " ops = " << total_ops
            << " total" << std::endl;
  std::cout << "[HighConcurrency50Threads] Successes: " << successes << " (" << success_rate << "%)" << std::endl;
  std::cout << "[HighConcurrency50Threads] Duration: " << duration_ms << " ms" << std::endl;

  // Verify > 80% success rate
  EXPECT_GT(success_rate, 80.0) << "Expected > 80% success rate, got " << success_rate << "%";
}
