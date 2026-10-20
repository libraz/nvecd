/**
 * @file dedup_cache_test.cpp
 * @brief Unit tests for DedupCache
 */

#include "events/dedup_cache.h"

#include <gtest/gtest.h>

#include <limits>
#include <thread>
#include <vector>

namespace nvecd::events {

class DedupCacheTest : public ::testing::Test {
 protected:
  void SetUp() override {}
};

TEST_F(DedupCacheTest, BasicDuplicateDetection) {
  DedupCache cache(100, 60);  // 100 entries, 60 second window

  EventKey key("ctx1", "id1", 100);

  // First insertion - not a duplicate
  EXPECT_FALSE(cache.CheckAndInsert(key, 1000));

  // Same event within window - duplicate
  EXPECT_TRUE(cache.WouldDeduplicate(key, 1010));
  EXPECT_TRUE(cache.WouldDeduplicate(key, 1059));

  // Same event at window boundary - duplicate
  EXPECT_TRUE(cache.WouldDeduplicate(key, 1060));

  // Same event outside window - not a duplicate
  EXPECT_FALSE(cache.WouldDeduplicate(key, 1061));
}

TEST_F(DedupCacheTest, DifferentKeysNotDuplicate) {
  DedupCache cache(100, 60);

  EventKey key1("ctx1", "id1", 100);
  EventKey key2("ctx1", "id2", 100);  // Different ID
  EventKey key3("ctx2", "id1", 100);  // Different context
  EventKey key4("ctx1", "id1", 200);  // Different score

  cache.CheckAndInsert(key1, 1000);

  // Different keys are not duplicates
  EXPECT_FALSE(cache.WouldDeduplicate(key2, 1000));
  EXPECT_FALSE(cache.WouldDeduplicate(key3, 1000));
  EXPECT_FALSE(cache.WouldDeduplicate(key4, 1000));
}

TEST_F(DedupCacheTest, LRUEviction) {
  DedupCache cache(3, 60);  // Small cache: 3 entries

  EventKey key1("ctx1", "id1", 100);
  EventKey key2("ctx1", "id2", 100);
  EventKey key3("ctx1", "id3", 100);
  EventKey key4("ctx1", "id4", 100);

  // Fill cache to capacity
  cache.CheckAndInsert(key1, 1000);
  cache.CheckAndInsert(key2, 1000);
  cache.CheckAndInsert(key3, 1000);
  EXPECT_EQ(cache.Size(), 3);

  // All three should be in cache
  EXPECT_TRUE(cache.WouldDeduplicate(key1, 1010));
  EXPECT_TRUE(cache.WouldDeduplicate(key2, 1010));
  EXPECT_TRUE(cache.WouldDeduplicate(key3, 1010));

  // Insert 4th entry - should evict key1 (least recently used)
  cache.CheckAndInsert(key4, 1000);
  EXPECT_EQ(cache.Size(), 3);

  // key1 should be evicted
  EXPECT_FALSE(cache.WouldDeduplicate(key1, 1010));

  // Others should still be in cache
  EXPECT_TRUE(cache.WouldDeduplicate(key2, 1010));
  EXPECT_TRUE(cache.WouldDeduplicate(key3, 1010));
  EXPECT_TRUE(cache.WouldDeduplicate(key4, 1010));
}

TEST_F(DedupCacheTest, UpdateMovesToFront) {
  DedupCache cache(3, 60);

  EventKey key1("ctx1", "id1", 100);
  EventKey key2("ctx1", "id2", 100);
  EventKey key3("ctx1", "id3", 100);
  EventKey key4("ctx1", "id4", 100);

  // Fill cache
  cache.CheckAndInsert(key1, 1000);
  cache.CheckAndInsert(key2, 1000);
  cache.CheckAndInsert(key3, 1000);

  // Access key1 (moves to front)
  cache.CheckAndInsert(key1, 1100);

  // Insert key4 - should evict key2 (now least recently used)
  cache.CheckAndInsert(key4, 1000);

  // key2 should be evicted
  EXPECT_FALSE(cache.WouldDeduplicate(key2, 1010));

  // key1 should still be in cache (was moved to front)
  EXPECT_TRUE(cache.WouldDeduplicate(key1, 1110));
}

TEST_F(DedupCacheTest, Clear) {
  DedupCache cache(100, 60);

  EventKey key1("ctx1", "id1", 100);
  EventKey key2("ctx1", "id2", 100);

  cache.CheckAndInsert(key1, 1000);
  cache.CheckAndInsert(key2, 1000);
  EXPECT_EQ(cache.Size(), 2);

  cache.Clear();
  EXPECT_EQ(cache.Size(), 0);

  // Nothing should be in cache after clear
  EXPECT_FALSE(cache.WouldDeduplicate(key1, 1010));
  EXPECT_FALSE(cache.WouldDeduplicate(key2, 1010));
}

TEST_F(DedupCacheTest, Statistics) {
  DedupCache cache(100, 60);

  EventKey key1("ctx1", "id1", 100);
  EventKey key2("ctx1", "id2", 100);

  // Initial stats
  auto stats = cache.GetStatistics();
  EXPECT_EQ(stats.size, 0);
  EXPECT_EQ(stats.max_size, 100);
  EXPECT_EQ(stats.total_hits, 0);
  EXPECT_EQ(stats.total_misses, 0);

  // First sighting - miss, and recorded
  EXPECT_FALSE(cache.CheckAndInsert(key1, 1000));
  stats = cache.GetStatistics();
  EXPECT_EQ(stats.size, 1);
  EXPECT_EQ(stats.total_misses, 1);

  // Duplicate within window - hit
  EXPECT_TRUE(cache.CheckAndInsert(key1, 1010));
  stats = cache.GetStatistics();
  EXPECT_EQ(stats.total_hits, 1);
  EXPECT_EQ(stats.total_misses, 1);

  // Different key - miss
  EXPECT_FALSE(cache.CheckAndInsert(key2, 1010));
  stats = cache.GetStatistics();
  EXPECT_EQ(stats.total_hits, 1);
  EXPECT_EQ(stats.total_misses, 2);

  // The read-only probe leaves the counters alone.
  EXPECT_TRUE(cache.WouldDeduplicate(key1, 1010));
  EXPECT_EQ(cache.GetStatistics().total_hits, 1);
}

TEST_F(DedupCacheTest, ZeroWindowDisabled) {
  DedupCache cache(100, 0);  // 0 second window = disabled

  EventKey key("ctx1", "id1", 100);

  // Even an immediate repeat is not detected (window = 0)
  EXPECT_FALSE(cache.CheckAndInsert(key, 1000));
  EXPECT_FALSE(cache.CheckAndInsert(key, 1000));
  EXPECT_FALSE(cache.WouldDeduplicate(key, 1000));
}

// Whether a record is a duplicate depends on event time alone, so a timestamp
// that jumps backwards beyond the window is a new event however soon it
// arrives. It does not pull the anchor back, so retries of the newer event
// stay deduplicated.
TEST_F(DedupCacheTest, BackdatedEventNeverMovesAnchorBackwards) {
  DedupCache cache(100, 60);
  EventKey key("ctx1", "id1", 100);

  EXPECT_FALSE(cache.CheckAndInsert(key, 1'000'000));
  EXPECT_FALSE(cache.CheckAndInsert(key, 1'000));
  EXPECT_TRUE(cache.CheckAndInsert(key, 1'000'000));
  EXPECT_TRUE(cache.CheckAndInsert(key, 1'000'030));
  EXPECT_EQ(cache.GetStatistics().total_hits, 2U);
}

TEST_F(DedupCacheTest, SeedAdvancesAnchorForwardOnlyAndSkipsCounters) {
  DedupCache cache(100, 60);
  EventKey key("ctx1", "id1", 100);

  cache.Seed(key, 5'000);
  EXPECT_TRUE(cache.WouldDeduplicate(key, 5'030));
  cache.Seed(key, 100);
  EXPECT_TRUE(cache.WouldDeduplicate(key, 5'030));
  EXPECT_FALSE(cache.WouldDeduplicate(key, 100));

  const auto stats = cache.GetStatistics();
  EXPECT_EQ(stats.size, 1U);
  EXPECT_EQ(stats.total_hits, 0U);
  EXPECT_EQ(stats.total_misses, 0U);
}

TEST_F(DedupCacheTest, SeedIsNoOpWhenWindowDisabled) {
  DedupCache cache(100, 0);
  cache.Seed(EventKey("ctx1", "id1", 100), 1'000);
  EXPECT_EQ(cache.Size(), 0U);
}

TEST_F(DedupCacheTest, MemoryUsageGrowsWithEntries) {
  DedupCache cache(100, 60);
  const size_t empty = cache.MemoryUsage();
  cache.Seed(EventKey("ctx1", "id1", 100), 1'000);
  EXPECT_GE(cache.MemoryUsage() - empty, 2 * sizeof(EventKey) + sizeof(void*));
}

TEST_F(DedupCacheTest, TimestampDistanceUsesOverflowSafeSubtraction) {
  DedupCache cache(100, 60);
  EventKey key("ctx1", "id1", 100);
  cache.CheckAndInsert(key, std::numeric_limits<uint64_t>::max());
  EXPECT_TRUE(cache.WouldDeduplicate(key, std::numeric_limits<uint64_t>::max() - 10));
  EXPECT_FALSE(cache.WouldDeduplicate(key, 0));
}

TEST_F(DedupCacheTest, ThreadSafety) {
  DedupCache cache(1000, 60);

  const int kNumThreads = 4;
  const int kOpsPerThread = 1000;

  std::vector<std::thread> threads;
  for (int t = 0; t < kNumThreads; ++t) {
    threads.emplace_back([&cache, t]() {
      for (int i = 0; i < kOpsPerThread; ++i) {
        EventKey key("ctx" + std::to_string(t), "id" + std::to_string(i % 100), i % 10);
        uint64_t ts = 1000 + (i / 10);

        // Mix of reads and writes
        cache.WouldDeduplicate(key, ts);
        cache.CheckAndInsert(key, ts);
      }
    });
  }

  for (auto& thread : threads) {
    thread.join();
  }

  // Should not crash and size should be within bounds
  auto stats = cache.GetStatistics();
  EXPECT_LE(stats.size, 1000);
}

}  // namespace nvecd::events
