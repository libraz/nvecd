/**
 * @file similarity_cache_test.cpp
 * @brief Unit tests for SimilarityCache efficiency
 *
 * Tests cache hit rates, eviction policies, and performance characteristics
 */

#include "cache/similarity_cache.h"

#include <gtest/gtest.h>

#include <thread>
#include <vector>

#include "cache/cache_key.h"

namespace nvecd::cache {

/**
 * @brief Test helper to access SimilarityCache internals
 */
class SimilarityCacheTestHelper {
 public:
  /// Corrupt the compressed_data of a cache entry to trigger decompression failure
  static bool CorruptEntry(SimilarityCache& cache, const CacheKey& key) {
    std::unique_lock lock(cache.mutex_);
    auto iter = cache.cache_map_.find(key);
    if (iter == cache.cache_map_.end()) {
      return false;
    }
    auto& data = iter->second.first.compressed_data;
    // Overwrite with invalid data
    for (auto& byte : data) {
      byte = 0xFF;
    }
    return true;
  }
};

namespace {

// Helper to create a simple cache key
CacheKey MakeKey(const std::string& id, int top_k) {
  std::string key_str = id + ":" + std::to_string(top_k);
  return CacheKeyGenerator::Generate(key_str);
}

// ============================================================================
// Basic Cache Operations
// ============================================================================

TEST(SimilarityCacheTest, ConstructEmpty) {
  SimilarityCache cache(1024 * 1024, 0.0);  // 1MB cache, no min cost
  auto stats = cache.GetStatistics();

  EXPECT_EQ(stats.total_queries, 0);
  EXPECT_EQ(stats.cache_hits, 0);
  EXPECT_EQ(stats.cache_misses, 0);
  EXPECT_EQ(stats.current_entries, 0);
  EXPECT_EQ(stats.current_memory_bytes, 0);
  EXPECT_EQ(stats.ttl_expirations, 0);
  EXPECT_EQ(stats.decompression_failures, 0);
}

TEST(SimilarityCacheTest, SingleQuery) {
  SimilarityCache cache(1024 * 1024, 0.0);

  auto key = MakeKey("item1", 10);

  std::vector<similarity::SimilarityResult> results = {{"item2", 0.95f}, {"item3", 0.90f}};

  // First lookup - miss
  auto retrieved1 = cache.Lookup(key);
  EXPECT_FALSE(retrieved1.has_value());

  // Insert
  bool inserted = cache.Insert(key, results, 1.5);  // 1.5ms query time
  EXPECT_TRUE(inserted);

  // Second lookup - hit
  auto retrieved2 = cache.Lookup(key);
  ASSERT_TRUE(retrieved2.has_value());
  EXPECT_EQ(retrieved2->size(), 2);
  EXPECT_EQ((*retrieved2)[0].item_id, "item2");

  auto stats = cache.GetStatistics();
  EXPECT_EQ(stats.total_queries, 2);  // 1 miss + 1 hit
  EXPECT_EQ(stats.cache_hits, 1);
  EXPECT_EQ(stats.cache_misses, 1);
}

// ============================================================================
// Cache Hit Rate Tests
// ============================================================================

TEST(SimilarityCacheTest, HitRate_RepeatedQueries) {
  SimilarityCache cache(1024 * 1024, 0.0);

  auto key = MakeKey("item1", 10);

  std::vector<similarity::SimilarityResult> results = {{"item2", 0.95f}};

  // First query - cache miss
  auto result1 = cache.Lookup(key);
  EXPECT_FALSE(result1.has_value());

  // Store result
  cache.Insert(key, results, 1.0);

  // Repeated queries - all hits
  const int repeat_count = 10;
  for (int i = 0; i < repeat_count; ++i) {
    auto result = cache.Lookup(key);
    ASSERT_TRUE(result.has_value()) << "Miss on iteration " << i;
  }

  auto stats = cache.GetStatistics();
  EXPECT_EQ(stats.total_queries, 1 + repeat_count);  // 1 miss + 10 hits
  EXPECT_EQ(stats.cache_hits, repeat_count);
  EXPECT_EQ(stats.cache_misses, 1);

  double expected_hit_rate = static_cast<double>(repeat_count) / (1 + repeat_count);
  EXPECT_FLOAT_EQ(stats.HitRate(), expected_hit_rate);
}

TEST(SimilarityCacheTest, HitRate_MultipleKeys) {
  SimilarityCache cache(1024 * 1024, 0.0);

  // Store 5 different queries
  for (int i = 0; i < 5; ++i) {
    auto key = MakeKey("item" + std::to_string(i), 10);

    std::vector<similarity::SimilarityResult> results = {{"result" + std::to_string(i), 0.95f}};

    // Miss
    auto result = cache.Lookup(key);
    EXPECT_FALSE(result.has_value());

    // Store
    cache.Insert(key, results, 1.0);

    // Hit
    auto result2 = cache.Lookup(key);
    ASSERT_TRUE(result2.has_value()) << "Cache miss on key " << i << " after insert";
    EXPECT_EQ((*result2)[0].item_id, "result" + std::to_string(i));
  }

  auto stats = cache.GetStatistics();
  EXPECT_EQ(stats.total_queries, 10);  // 5 misses + 5 hits
  EXPECT_EQ(stats.cache_hits, 5);
  EXPECT_EQ(stats.cache_misses, 5);
  EXPECT_FLOAT_EQ(stats.HitRate(), 0.5);
}

TEST(SimilarityCacheTest, HitRate_WorkloadSimulation) {
  // Simulate realistic workload: 20% unique queries, 80% repeats
  SimilarityCache cache(10 * 1024 * 1024, 0.0);  // 10MB

  const int unique_queries = 100;
  const int total_queries = 500;

  // Add unique queries
  for (int i = 0; i < unique_queries; ++i) {
    auto key = MakeKey("item" + std::to_string(i), 10);

    std::vector<similarity::SimilarityResult> results = {{"result" + std::to_string(i), 0.95f}};

    cache.Insert(key, results, 1.0);
  }

  // Execute queries (80% hit expected)
  int expected_hits = 0;
  int expected_misses = 0;
  for (int i = 0; i < total_queries; ++i) {
    // 80% of queries hit existing keys (0-99), 20% miss (100+)
    int id = (i % 5 == 0) ? (100 + i) : (i % unique_queries);
    auto key = MakeKey("item" + std::to_string(id), 10);

    auto result = cache.Lookup(key);
    if (id < unique_queries) {
      // This key was inserted, so it must be a hit
      ASSERT_TRUE(result.has_value()) << "Expected hit for item" << id;
      ++expected_hits;
    } else {
      // This key was never inserted, so it must be a miss
      EXPECT_FALSE(result.has_value()) << "Expected miss for item" << id;
      ++expected_misses;
    }
  }

  auto stats = cache.GetStatistics();
  EXPECT_EQ(stats.cache_hits, static_cast<uint64_t>(expected_hits));
  EXPECT_EQ(stats.cache_misses, static_cast<uint64_t>(expected_misses));
  EXPECT_EQ(stats.total_queries, static_cast<uint64_t>(total_queries));
  double expected_hit_rate = 0.8;
  EXPECT_NEAR(stats.HitRate(), expected_hit_rate, 0.05);
  EXPECT_GT(stats.HitRate(), 0.75);  // Should be at least 75%
}

// ============================================================================
// Cache Eviction Tests
// ============================================================================

TEST(SimilarityCacheTest, Eviction_MemoryLimit) {
  // Small cache: 5KB (will hold ~10-15 entries with compression)
  SimilarityCache cache(5 * 1024, 0.0);

  const int entry_count = 100;
  std::vector<similarity::SimilarityResult> large_results(20);  // Increased size
  for (int i = 0; i < 20; ++i) {
    large_results[i] = {"result_with_longer_id_" + std::to_string(i), static_cast<float>(i) * 0.1f};
  }

  // Fill cache beyond capacity
  int successful_inserts = 0;
  for (int i = 0; i < entry_count; ++i) {
    auto key = MakeKey("item" + std::to_string(i), 10);
    if (cache.Insert(key, large_results, 1.0)) {
      successful_inserts++;
    }
  }

  auto stats = cache.GetStatistics();
  EXPECT_LT(stats.current_entries, entry_count);    // Some entries should be evicted
  EXPECT_LE(stats.current_memory_bytes, 5 * 1024);  // Should not exceed limit
  // Either evictions occurred, or some inserts were rejected due to size
  EXPECT_TRUE(stats.evictions > 0 || successful_inserts < entry_count);
}

TEST(SimilarityCacheTest, Eviction_LRUPolicy) {
  // Small cache
  SimilarityCache cache(2048, 0.0);

  std::vector<similarity::SimilarityResult> results = {{"result", 0.95f}};

  // Add 3 entries
  auto key1 = MakeKey("item1", 10);
  cache.Insert(key1, results, 1.0);

  auto key2 = MakeKey("item2", 10);
  cache.Insert(key2, results, 1.0);

  auto key3 = MakeKey("item3", 10);
  cache.Insert(key3, results, 1.0);

  // Access item1 to make it recently used
  auto result1 = cache.Lookup(key1);
  EXPECT_TRUE(result1.has_value());

  // Add many more entries to force eviction
  for (int i = 10; i < 100; ++i) {
    auto key = MakeKey("item" + std::to_string(i), 10);
    cache.Insert(key, results, 1.0);
  }

  auto stats = cache.GetStatistics();
  EXPECT_GT(stats.evictions, 0);

  // item1 was accessed most recently before the bulk inserts, so under LRU
  // policy it should be retained longer than item2/item3. However, with a
  // very small cache (2048 bytes) and 90+ subsequent inserts, even item1 may
  // be evicted. We assert the weaker invariant: if the cache still holds
  // entries from the original set, item1 (the most recently accessed) should
  // be among them.
  auto still_there_1 = cache.Lookup(key1);
  auto still_there_2 = cache.Lookup(key2);
  auto still_there_3 = cache.Lookup(key3);
  if (still_there_2.has_value() || still_there_3.has_value()) {
    // If less-recently-used items survived, the most-recently-used must too
    EXPECT_TRUE(still_there_1.has_value())
        << "LRU violation: item2 or item3 survived eviction but item1 "
           "(most recently accessed) did not";
  }
}

// ============================================================================
// Invalidation Tests
// ============================================================================

TEST(SimilarityCacheTest, Invalidate_SingleKey) {
  SimilarityCache cache(1024 * 1024, 0.0);

  // Add multiple entries
  auto key1 = MakeKey("item1", 10);
  auto key2 = MakeKey("item2", 10);

  std::vector<similarity::SimilarityResult> results = {{"result", 0.95f}};
  cache.Insert(key1, results, 1.0);
  cache.Insert(key2, results, 1.0);

  auto stats_before = cache.GetStatistics();
  EXPECT_EQ(stats_before.current_entries, 2);

  // Erase item1
  bool erased = cache.Erase(key1);
  EXPECT_TRUE(erased);

  // item1 should be gone, item2 should remain
  auto result1 = cache.Lookup(key1);
  EXPECT_FALSE(result1.has_value());

  auto result2 = cache.Lookup(key2);
  EXPECT_TRUE(result2.has_value());

  auto stats_after = cache.GetStatistics();
  EXPECT_EQ(stats_after.current_entries, 1);
}

TEST(SimilarityCacheTest, Invalidate_ClearAll) {
  SimilarityCache cache(1024 * 1024, 0.0);

  // Add 10 entries
  std::vector<similarity::SimilarityResult> results = {{"result", 0.95f}};
  std::vector<CacheKey> keys;

  for (int i = 0; i < 10; ++i) {
    auto key = MakeKey("item" + std::to_string(i), 10);
    keys.push_back(key);
    cache.Insert(key, results, 1.0);
  }

  auto stats_before = cache.GetStatistics();
  EXPECT_EQ(stats_before.current_entries, 10);

  // Clear all
  cache.Clear();

  auto stats_after = cache.GetStatistics();
  EXPECT_EQ(stats_after.current_entries, 0);
  EXPECT_EQ(stats_after.current_memory_bytes, 0);

  // Verify all entries are gone
  for (const auto& key : keys) {
    auto result = cache.Lookup(key);
    EXPECT_FALSE(result.has_value());
  }
}

// ============================================================================
// TTL Expiration Tests
// ============================================================================

TEST(SimilarityCacheTest, TtlExpiration_RemovesExpiredEntry) {
  // TTL of 1 second
  SimilarityCache cache(1024 * 1024, 0.0, 1);

  auto key = MakeKey("item1", 10);
  std::vector<similarity::SimilarityResult> results = {{"item2", 0.95f}};

  // Insert entry
  bool inserted = cache.Insert(key, results, 1.0);
  EXPECT_TRUE(inserted);

  // Verify entry exists
  auto stats_before = cache.GetStatistics();
  EXPECT_EQ(stats_before.current_entries, 1);

  // Wait for TTL to expire
  std::this_thread::sleep_for(std::chrono::milliseconds(1100));

  // Lookup should return nullopt (expired)
  auto result = cache.Lookup(key);
  EXPECT_FALSE(result.has_value());

  // Entry should have been removed from cache
  auto stats_after = cache.GetStatistics();
  EXPECT_EQ(stats_after.current_entries, 0);
  EXPECT_EQ(stats_after.ttl_expirations, 1);
}

TEST(SimilarityCacheTest, TtlExpiration_StatsTracked) {
  SimilarityCache cache(1024 * 1024, 0.0, 1);

  // Insert multiple entries
  for (int i = 0; i < 3; ++i) {
    auto key = MakeKey("item" + std::to_string(i), 10);
    std::vector<similarity::SimilarityResult> results = {{"result" + std::to_string(i), 0.95f}};
    cache.Insert(key, results, 1.0);
  }

  EXPECT_EQ(cache.GetStatistics().current_entries, 3);

  // Wait for TTL to expire
  std::this_thread::sleep_for(std::chrono::milliseconds(1100));

  // Lookup all expired entries
  for (int i = 0; i < 3; ++i) {
    auto key = MakeKey("item" + std::to_string(i), 10);
    auto result = cache.Lookup(key);
    EXPECT_FALSE(result.has_value());
  }

  auto stats = cache.GetStatistics();
  EXPECT_EQ(stats.current_entries, 0);
  EXPECT_EQ(stats.ttl_expirations, 3);
}

// ============================================================================
// Min Query Cost Tests
// ============================================================================

TEST(SimilarityCacheTest, MinQueryCost_OnlySlowQueries) {
  SimilarityCache cache(1024 * 1024, 2.0);  // Only cache queries >= 2.0ms

  auto key1 = MakeKey("slow_query", 10);
  auto key2 = MakeKey("fast_query", 10);

  std::vector<similarity::SimilarityResult> results = {{"result", 0.95f}};

  // Fast query (should not be cached)
  bool fast_inserted = cache.Insert(key2, results, 0.5);  // 0.5ms < 2.0ms threshold
  EXPECT_FALSE(fast_inserted);

  // Slow query (should be cached)
  bool slow_inserted = cache.Insert(key1, results, 3.0);  // 3.0ms >= 2.0ms threshold
  EXPECT_TRUE(slow_inserted);

  auto stats = cache.GetStatistics();
  EXPECT_EQ(stats.current_entries, 1);  // Only slow query cached

  // Verify
  auto fast_result = cache.Lookup(key2);
  EXPECT_FALSE(fast_result.has_value());

  auto slow_result = cache.Lookup(key1);
  EXPECT_TRUE(slow_result.has_value());
}

// ============================================================================
// Decompression Failure Tests
// ============================================================================

TEST(SimilarityCacheTest, DecompressionFailure_ErasesCorruptedEntry) {
  SimilarityCache cache(1024 * 1024, 0.0);

  auto key = MakeKey("item1", 10);
  std::vector<similarity::SimilarityResult> results = {{"item2", 0.95f}, {"item3", 0.90f}};

  // First lookup - miss
  auto result1 = cache.Lookup(key);
  EXPECT_FALSE(result1.has_value());

  // Insert and verify normal lookup works
  bool inserted = cache.Insert(key, results, 1.5);
  EXPECT_TRUE(inserted);

  auto result2 = cache.Lookup(key);
  ASSERT_TRUE(result2.has_value());
  EXPECT_EQ(result2->size(), 2);

  // Corrupt the entry's compressed data
  bool corrupted = SimilarityCacheTestHelper::CorruptEntry(cache, key);
  EXPECT_TRUE(corrupted);

  // Lookup should fail gracefully (decompression failure)
  auto result3 = cache.Lookup(key);
  EXPECT_FALSE(result3.has_value());

  auto stats = cache.GetStatistics();
  EXPECT_EQ(stats.decompression_failures, 1);
  // Issue 1 fix: cache_hits should be 1 (only the successful lookup)
  EXPECT_EQ(stats.cache_hits, 1);
  // Issue 2 fix: cache_misses == initial miss + decompression failure
  EXPECT_EQ(stats.cache_misses, 2);
  // Issue 2 fix: cache_misses_not_found == initial miss + decompression failure
  EXPECT_EQ(stats.cache_misses_not_found, 2);
  // Corrupted entry should have been erased
  EXPECT_EQ(stats.current_entries, 0);
}

// ============================================================================
// Concurrent Access Tests
// ============================================================================

TEST(SimilarityCacheTest, ConcurrentReadsAndWrites) {
  SimilarityCache cache(10 * 1024 * 1024, 0.0);

  std::vector<similarity::SimilarityResult> results = {{"result", 0.95f}};

  // Writer thread: add entries
  std::thread writer([&cache, &results]() {
    for (int i = 0; i < 100; ++i) {
      auto key = MakeKey("item" + std::to_string(i), 10);
      cache.Insert(key, results, 1.0);
    }
  });

  // Reader threads: query entries
  std::vector<std::thread> readers;
  std::atomic<int> total_lookups{0};
  std::atomic<int> total_hits{0};
  for (int t = 0; t < 4; ++t) {
    readers.emplace_back([&cache, &total_lookups, &total_hits]() {
      for (int i = 0; i < 100; ++i) {
        auto key = MakeKey("item" + std::to_string(i % 50), 10);  // Query first 50 items
        auto result = cache.Lookup(key);
        total_lookups.fetch_add(1, std::memory_order_relaxed);
        // Hit/miss depends on whether writer has inserted this key yet
        if (result.has_value()) {
          total_hits.fetch_add(1, std::memory_order_relaxed);
        }
      }
    });
  }

  writer.join();
  for (auto& reader : readers) {
    reader.join();
  }

  auto stats = cache.GetStatistics();
  // 4 reader threads x 100 lookups = 400 reader queries
  EXPECT_EQ(total_lookups.load(), 400);
  // Stats must reflect all queries (writer inserts don't count as queries)
  EXPECT_EQ(stats.total_queries, 400);
  // Hits + misses must account for all queries
  EXPECT_EQ(stats.cache_hits + stats.cache_misses, stats.total_queries);
  // Some hits should occur since writer and readers run concurrently
  // (writer inserts 100 items while readers query 50 items repeatedly)
  EXPECT_GT(stats.cache_hits, 0);
}

// ============================================================================
// Phase 1: Concurrent SetMinQueryCost + Insert (data race fix verification)
// ============================================================================

TEST(SimilarityCacheTest, ConcurrentSetMinQueryCostAndInsert) {
  SimilarityCache cache(10 * 1024 * 1024, 0.0);

  std::atomic<bool> stop{false};
  std::vector<similarity::SimilarityResult> results = {{"result", 0.95f}};

  // 1 thread: SetMinQueryCost() loop with varying values
  std::thread config_writer([&cache, &stop]() {
    double val = 0.0;
    while (!stop.load(std::memory_order_relaxed)) {
      cache.SetMinQueryCost(val);
      val += 0.1;
      if (val > 10.0)
        val = 0.0;
      std::this_thread::yield();
    }
  });

  // 4 threads: Insert() concurrently with varying query costs
  std::vector<std::thread> inserters;
  for (int t = 0; t < 4; ++t) {
    inserters.emplace_back([&cache, &results, &stop, t]() {
      int i = 0;
      while (!stop.load(std::memory_order_relaxed)) {
        auto key = MakeKey("t" + std::to_string(t) + "_item" + std::to_string(i), 10);
        double cost = static_cast<double>(i % 20) * 0.5;
        cache.Insert(key, results, cost);
        ++i;
        std::this_thread::yield();
      }
    });
  }

  // Run for 100ms
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  stop.store(true, std::memory_order_relaxed);

  config_writer.join();
  for (auto& t : inserters) {
    t.join();
  }

  // Should complete without crash or TSAN violations
  auto stats = cache.GetStatistics();
  // Some inserts must have succeeded (4 threads inserting continuously for 100ms)
  EXPECT_GT(stats.current_entries, 0);
  // Memory tracking must remain consistent
  EXPECT_GT(stats.current_memory_bytes, 0);
}

// ============================================================================
// Phase 2: Stats consistency tests
// ============================================================================

TEST(SimilarityCacheTest, ClearIfStatsConsistency) {
  SimilarityCache cache(1024 * 1024, 0.0);

  std::vector<similarity::SimilarityResult> results = {{"result", 0.95f}};

  // Insert 10 entries
  for (int i = 0; i < 10; ++i) {
    auto key = MakeKey("item" + std::to_string(i), 10);
    cache.Insert(key, results, 1.0);
  }

  auto stats_before = cache.GetStatistics();
  EXPECT_EQ(stats_before.current_entries, 10);

  // ClearIf: remove entries with even hash_high values (roughly half)
  cache.ClearIf([](const CacheKey& key) { return (key.hash_high % 2) == 0; });

  auto stats_after = cache.GetStatistics();
  // Entries should be fewer
  EXPECT_LT(stats_after.current_entries, stats_before.current_entries);
  // Memory should be consistent
  EXPECT_LT(stats_after.current_memory_bytes, stats_before.current_memory_bytes);

  // Verify that looking up remaining entries works
  uint64_t found_count = 0;
  for (int i = 0; i < 10; ++i) {
    auto key = MakeKey("item" + std::to_string(i), 10);
    auto result = cache.Lookup(key);
    if (result.has_value()) {
      ++found_count;
    }
  }
  EXPECT_EQ(found_count, stats_after.current_entries);
}

TEST(SimilarityCacheTest, EvictForSpaceStatsConsistency) {
  // Small cache: 2KB
  SimilarityCache cache(2048, 0.0);

  std::vector<similarity::SimilarityResult> results = {{"result_id", 0.95f}};

  int successful_inserts = 0;
  for (int i = 0; i < 50; ++i) {
    auto key = MakeKey("item" + std::to_string(i), 10);
    if (cache.Insert(key, results, 1.0)) {
      successful_inserts++;
    }
  }

  auto stats = cache.GetStatistics();
  // Memory should not exceed limit
  EXPECT_LE(stats.current_memory_bytes, 2048);
  // Should have evictions if we inserted more than fits
  if (successful_inserts > static_cast<int>(stats.current_entries)) {
    EXPECT_GT(stats.evictions, 0);
  }
  // current_entries + evictions should account for all successful inserts
  EXPECT_EQ(stats.current_entries + stats.evictions, static_cast<uint64_t>(successful_inserts));
}

// ============================================================================
// Phase 3: Edge case concurrent tests
// ============================================================================

TEST(SimilarityCacheTest, ConcurrentMarkInvalidatedAndLookup) {
  SimilarityCache cache(10 * 1024 * 1024, 0.0);

  std::vector<similarity::SimilarityResult> results = {{"result", 0.95f}};

  // Pre-populate cache
  for (int i = 0; i < 100; ++i) {
    auto key = MakeKey("item" + std::to_string(i), 10);
    cache.Insert(key, results, 1.0);
  }

  std::atomic<bool> stop{false};

  // 2 threads: MarkInvalidated() on random keys
  std::vector<std::thread> invalidators;
  for (int t = 0; t < 2; ++t) {
    invalidators.emplace_back([&cache, &stop]() {
      int i = 0;
      while (!stop.load(std::memory_order_relaxed)) {
        auto key = MakeKey("item" + std::to_string(i % 100), 10);
        cache.MarkInvalidated(key);
        ++i;
        std::this_thread::yield();
      }
    });
  }

  // 4 threads: Lookup() concurrently
  std::vector<std::thread> readers;
  for (int t = 0; t < 4; ++t) {
    readers.emplace_back([&cache, &stop]() {
      int i = 0;
      while (!stop.load(std::memory_order_relaxed)) {
        auto key = MakeKey("item" + std::to_string(i % 100), 10);
        auto result = cache.Lookup(key);
        (void)result;
        ++i;
        std::this_thread::yield();
      }
    });
  }

  // Run for 100ms
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  stop.store(true, std::memory_order_relaxed);

  for (auto& t : invalidators) {
    t.join();
  }
  for (auto& t : readers) {
    t.join();
  }

  // Should complete without crash or TSAN violations
  auto stats = cache.GetStatistics();
  // Invalidators ran concurrently with lookups, so some lookups must have
  // encountered invalidated entries. Both hits and misses should be recorded.
  EXPECT_GT(stats.total_queries, 0);
  EXPECT_EQ(stats.cache_hits + stats.cache_misses, stats.total_queries);
  // At least some lookups must have missed due to invalidation
  EXPECT_GT(stats.cache_misses, 0);
  // cache_misses_invalidated should be > 0 since invalidators ran continuously
  EXPECT_GT(stats.cache_misses_invalidated, 0);
}

TEST(SimilarityCacheTest, EvictForSpaceFailsGracefully) {
  // Extremely small cache: 1 byte — nothing can fit
  SimilarityCache cache(1, 0.0);

  std::vector<similarity::SimilarityResult> results = {{"result_with_data", 0.95f}};

  // Insert should fail gracefully (entry too large for cache)
  auto key = MakeKey("item1", 10);
  bool inserted = cache.Insert(key, results, 1.0);
  EXPECT_FALSE(inserted);

  auto stats = cache.GetStatistics();
  EXPECT_EQ(stats.current_entries, 0);
  EXPECT_EQ(stats.current_memory_bytes, 0);
}

TEST(SimilarityCacheTest, PurgeExpired_RemovesAllExpired) {
  // TTL of 1 second
  SimilarityCache cache(1024 * 1024, 0.0, 1);

  std::vector<similarity::SimilarityResult> results = {{"result", 0.95f}};

  // Insert multiple entries
  for (int i = 0; i < 5; ++i) {
    auto key = MakeKey("item" + std::to_string(i), 10);
    cache.Insert(key, results, 1.0);
  }

  EXPECT_EQ(cache.GetStatistics().current_entries, 5);

  // Wait for TTL to expire
  std::this_thread::sleep_for(std::chrono::milliseconds(1100));

  // PurgeExpired should remove all entries
  size_t purged = cache.PurgeExpired();
  EXPECT_EQ(purged, 5);
  EXPECT_EQ(cache.GetStatistics().current_entries, 0);
}

TEST(SimilarityCacheTest, PurgeExpired_NoTTL) {
  // No TTL
  SimilarityCache cache(1024 * 1024, 0.0, 0);

  std::vector<similarity::SimilarityResult> results = {{"result", 0.95f}};
  auto key = MakeKey("item1", 10);
  cache.Insert(key, results, 1.0);

  // PurgeExpired should return 0 when no TTL configured
  size_t purged = cache.PurgeExpired();
  EXPECT_EQ(purged, 0);
  EXPECT_EQ(cache.GetStatistics().current_entries, 1);
}

TEST(SimilarityCacheTest, PurgeExpired_MixedFreshAndExpired) {
  // TTL of 1 second
  SimilarityCache cache(1024 * 1024, 0.0, 1);

  std::vector<similarity::SimilarityResult> results = {{"result", 0.95f}};

  // Insert entries that will expire
  for (int i = 0; i < 3; ++i) {
    auto key = MakeKey("old_item" + std::to_string(i), 10);
    cache.Insert(key, results, 1.0);
  }

  // Wait for TTL
  std::this_thread::sleep_for(std::chrono::milliseconds(1100));

  // Insert fresh entries
  for (int i = 0; i < 2; ++i) {
    auto key = MakeKey("fresh_item" + std::to_string(i), 10);
    cache.Insert(key, results, 1.0);
  }

  EXPECT_EQ(cache.GetStatistics().current_entries, 5);

  // PurgeExpired should only remove old entries
  size_t purged = cache.PurgeExpired();
  EXPECT_EQ(purged, 3);
  EXPECT_EQ(cache.GetStatistics().current_entries, 2);
}

}  // namespace
}  // namespace nvecd::cache
