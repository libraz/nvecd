/**
 * @file server_stats_test.cpp
 * @brief Unit tests for ServerStats
 *
 * Reference: ../mygram-db/tests/server/server_stats_test.cpp
 * Reusability: 70% (adapted for nvecd command types)
 */

#include <gtest/gtest.h>

#include <thread>
#include <vector>

#include "server/server_types.h"

using namespace nvecd::server;

/**
 * @brief Test fixture for ServerStats tests
 */
class ServerStatsTest : public ::testing::Test {
 protected:
  void SetUp() override { stats_ = std::make_unique<ServerStats>(); }

  std::unique_ptr<ServerStats> stats_;
};

/**
 * @brief Test that statistics counters are initialized to zero
 */
TEST_F(ServerStatsTest, InitializedToZero) {
  EXPECT_EQ(stats_->total_connections, 0);
  EXPECT_EQ(stats_->active_connections, 0);
  EXPECT_EQ(stats_->total_commands, 0);
  EXPECT_EQ(stats_->failed_commands, 0);
  EXPECT_EQ(stats_->event_commands, 0);
  EXPECT_EQ(stats_->sim_commands, 0);
  EXPECT_EQ(stats_->vecset_commands, 0);
  EXPECT_EQ(stats_->info_commands, 0);
  EXPECT_EQ(stats_->config_commands, 0);
  EXPECT_EQ(stats_->dump_commands, 0);
  EXPECT_EQ(stats_->cache_commands, 0);
}

/**
 * @brief Test start time is reasonable
 */
TEST_F(ServerStatsTest, StartTimeReasonable) {
  uint64_t now = static_cast<uint64_t>(std::time(nullptr));
  EXPECT_LE(stats_->start_time, now);
  EXPECT_GT(stats_->start_time, now - 10);  // Should be within last 10 seconds
}

/**
 * @brief Test uptime calculation
 */
TEST_F(ServerStatsTest, UptimeCalculation) {
  // Initial uptime should be very small (nearly 0)
  uint64_t uptime1 = stats_->GetUptimeSeconds();
  EXPECT_LE(uptime1, 1);

  // Wait a bit and check uptime increased
  std::this_thread::sleep_for(std::chrono::seconds(1));
  uint64_t uptime2 = stats_->GetUptimeSeconds();
  EXPECT_GE(uptime2, 1);
  EXPECT_LE(uptime2, 2);
}

/**
 * @brief Test connection statistics
 */
TEST_F(ServerStatsTest, ConnectionStats) {
  stats_->total_connections++;
  EXPECT_EQ(stats_->total_connections, 1);

  stats_->active_connections++;
  EXPECT_EQ(stats_->active_connections, 1);

  stats_->total_connections++;
  stats_->total_connections++;
  EXPECT_EQ(stats_->total_connections, 3);

  stats_->active_connections++;
  EXPECT_EQ(stats_->active_connections, 2);

  stats_->active_connections--;
  EXPECT_EQ(stats_->active_connections, 1);
}

/**
 * @brief Test command statistics
 */
TEST_F(ServerStatsTest, CommandStats) {
  stats_->total_commands++;
  EXPECT_EQ(stats_->total_commands, 1);

  stats_->event_commands++;
  stats_->event_commands++;
  EXPECT_EQ(stats_->event_commands, 2);

  stats_->sim_commands++;
  EXPECT_EQ(stats_->sim_commands, 1);

  stats_->vecset_commands++;
  stats_->vecset_commands++;
  stats_->vecset_commands++;
  EXPECT_EQ(stats_->vecset_commands, 3);

  stats_->info_commands++;
  EXPECT_EQ(stats_->info_commands, 1);

  stats_->config_commands++;
  EXPECT_EQ(stats_->config_commands, 1);

  stats_->dump_commands++;
  EXPECT_EQ(stats_->dump_commands, 1);

  stats_->cache_commands++;
  EXPECT_EQ(stats_->cache_commands, 1);

  stats_->failed_commands++;
  stats_->failed_commands++;
  EXPECT_EQ(stats_->failed_commands, 2);
}

/**
 * @brief Test queries per second calculation
 */
TEST_F(ServerStatsTest, QueriesPerSecond) {
  // Initial QPS should be 0 (uptime ~0)
  double qps1 = stats_->GetQueriesPerSecond();
  EXPECT_GE(qps1, 0.0);

  // Add some commands and wait
  for (int i = 0; i < 100; ++i) {
    stats_->total_commands++;
  }

  std::this_thread::sleep_for(std::chrono::seconds(1));

  double qps2 = stats_->GetQueriesPerSecond();
  // Should be around 100 commands / 1 second = 100 QPS (allow some variance)
  EXPECT_GT(qps2, 50.0);
  EXPECT_LT(qps2, 200.0);
}

/**
 * @brief Test thread safety of statistics counters
 */
TEST_F(ServerStatsTest, ThreadSafety) {
  constexpr int kNumThreads = 10;
  constexpr int kIncrementsPerThread = 1000;

  std::vector<std::thread> threads;

  // Launch threads that increment different counters
  for (int i = 0; i < kNumThreads; ++i) {
    threads.emplace_back([this]() {
      for (int j = 0; j < kIncrementsPerThread; ++j) {
        stats_->total_commands++;
        stats_->event_commands++;
        stats_->sim_commands++;
        stats_->vecset_commands++;
      }
    });
  }

  // Wait for all threads
  for (auto& thread : threads) {
    thread.join();
  }

  // Verify counts
  EXPECT_EQ(stats_->total_commands, kNumThreads * kIncrementsPerThread);
  EXPECT_EQ(stats_->event_commands, kNumThreads * kIncrementsPerThread);
  EXPECT_EQ(stats_->sim_commands, kNumThreads * kIncrementsPerThread);
  EXPECT_EQ(stats_->vecset_commands, kNumThreads * kIncrementsPerThread);
}

/**
 * @brief Test combined statistics scenario
 */
TEST_F(ServerStatsTest, CombinedStatisticsScenario) {
  // Simulate a real workload:
  // - 100 connections (50 active)
  // - 1000 EVENT commands
  // - 500 SIM commands
  // - 300 VECSET commands
  // - 10 INFO commands
  // - 5 CONFIG commands
  // - 2 DUMP commands
  // - 20 failed commands

  for (int i = 0; i < 100; ++i) {
    stats_->total_connections++;
  }
  for (int i = 0; i < 50; ++i) {
    stats_->active_connections++;
  }

  for (int i = 0; i < 1000; ++i) {
    stats_->event_commands++;
    stats_->total_commands++;
  }

  for (int i = 0; i < 500; ++i) {
    stats_->sim_commands++;
    stats_->total_commands++;
  }

  for (int i = 0; i < 300; ++i) {
    stats_->vecset_commands++;
    stats_->total_commands++;
  }

  for (int i = 0; i < 10; ++i) {
    stats_->info_commands++;
    stats_->total_commands++;
  }

  for (int i = 0; i < 5; ++i) {
    stats_->config_commands++;
    stats_->total_commands++;
  }

  for (int i = 0; i < 2; ++i) {
    stats_->dump_commands++;
    stats_->total_commands++;
  }

  for (int i = 0; i < 20; ++i) {
    stats_->failed_commands++;
  }

  // Verify all counts
  EXPECT_EQ(stats_->total_connections, 100);
  EXPECT_EQ(stats_->active_connections, 50);
  EXPECT_EQ(stats_->total_commands, 1817);  // 1000+500+300+10+5+2
  EXPECT_EQ(stats_->event_commands, 1000);
  EXPECT_EQ(stats_->sim_commands, 500);
  EXPECT_EQ(stats_->vecset_commands, 300);
  EXPECT_EQ(stats_->info_commands, 10);
  EXPECT_EQ(stats_->config_commands, 5);
  EXPECT_EQ(stats_->dump_commands, 2);
  EXPECT_EQ(stats_->failed_commands, 20);
}

/**
 * @brief Test atomic load operations
 */
TEST_F(ServerStatsTest, AtomicLoadOperations) {
  stats_->total_commands = 100;
  stats_->event_commands = 50;

  uint64_t total = stats_->total_commands.load();
  uint64_t events = stats_->event_commands.load();

  EXPECT_EQ(total, 100);
  EXPECT_EQ(events, 50);
}

/**
 * @brief Test concurrent increments and reads
 */
TEST_F(ServerStatsTest, ConcurrentIncrementsAndReads) {
  constexpr int kNumWriters = 5;
  constexpr int kNumReaders = 5;
  constexpr int kIncrementsPerWriter = 1000;
  constexpr int kReadsPerReader = 1000;

  std::vector<std::thread> threads;

  // Launch writer threads
  for (int i = 0; i < kNumWriters; ++i) {
    threads.emplace_back([this]() {
      for (int j = 0; j < kIncrementsPerWriter; ++j) {
        stats_->total_commands++;
      }
    });
  }

  // Launch reader threads
  for (int i = 0; i < kNumReaders; ++i) {
    threads.emplace_back([this]() {
      for (int j = 0; j < kReadsPerReader; ++j) {
        [[maybe_unused]] uint64_t value = stats_->total_commands.load();
      }
    });
  }

  // Wait for all threads
  for (auto& thread : threads) {
    thread.join();
  }

  // Verify final count
  EXPECT_EQ(stats_->total_commands, kNumWriters * kIncrementsPerWriter);
}
