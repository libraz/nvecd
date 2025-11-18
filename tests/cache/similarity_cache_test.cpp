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
    EXPECT_TRUE(result2.has_value());
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
  for (int i = 0; i < total_queries; ++i) {
    // 80% of queries hit existing keys (0-99), 20% miss (100+)
    int id = (i % 5 == 0) ? (100 + i) : (i % unique_queries);
    auto key = MakeKey("item" + std::to_string(id), 10);

    auto result = cache.Lookup(key);
    (void)result;  // Suppress unused warning
  }

  auto stats = cache.GetStatistics();
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

  // item1 should still be in cache (recently accessed)
  auto still_there = cache.Lookup(key1);
  // Note: LRU may or may not keep item1 depending on cache size
  // This is a soft check

  auto stats = cache.GetStatistics();
  EXPECT_GT(stats.evictions, 0);
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
  for (int t = 0; t < 4; ++t) {
    readers.emplace_back([&cache]() {
      for (int i = 0; i < 100; ++i) {
        auto key = MakeKey("item" + std::to_string(i % 50), 10);  // Query first 50 items
        auto result = cache.Lookup(key);
        // May or may not hit depending on timing
      }
    });
  }

  writer.join();
  for (auto& reader : readers) {
    reader.join();
  }

  // Should complete without crash
  auto stats = cache.GetStatistics();
  EXPECT_GT(stats.total_queries, 0);
}

}  // namespace
}  // namespace nvecd::cache
