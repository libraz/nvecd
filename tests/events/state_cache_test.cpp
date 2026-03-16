/**
 * @file state_cache_test.cpp
 * @brief Unit tests for StateCache LRU eviction
 */

#include "events/state_cache.h"

#include <gtest/gtest.h>

namespace nvecd::events {
namespace {

// ============================================================================
// Basic Operations
// ============================================================================

TEST(StateCacheTest, ConstructEmpty) {
  StateCache cache(100);
  EXPECT_EQ(cache.Size(), 0);
}

TEST(StateCacheTest, IsDuplicateSetNewKey) {
  StateCache cache(100);
  StateKey key("ctx1", "item1");

  EXPECT_FALSE(cache.IsDuplicateSet(key, 100));
}

TEST(StateCacheTest, IsDuplicateSetSameScore) {
  StateCache cache(100);
  StateKey key("ctx1", "item1");

  cache.UpdateScore(key, 100);
  EXPECT_TRUE(cache.IsDuplicateSet(key, 100));
}

TEST(StateCacheTest, IsDuplicateSetDifferentScore) {
  StateCache cache(100);
  StateKey key("ctx1", "item1");

  cache.UpdateScore(key, 100);
  EXPECT_FALSE(cache.IsDuplicateSet(key, 200));
}

TEST(StateCacheTest, IsDuplicateDelNotTracked) {
  StateCache cache(100);
  StateKey key("ctx1", "item1");

  EXPECT_FALSE(cache.IsDuplicateDel(key));
}

TEST(StateCacheTest, IsDuplicateDelAlreadyDeleted) {
  StateCache cache(100);
  StateKey key("ctx1", "item1");

  cache.MarkDeleted(key);
  EXPECT_TRUE(cache.IsDuplicateDel(key));
}

TEST(StateCacheTest, ClearResetsAll) {
  StateCache cache(100);
  StateKey key("ctx1", "item1");

  cache.UpdateScore(key, 100);
  EXPECT_EQ(cache.Size(), 1);

  cache.Clear();
  EXPECT_EQ(cache.Size(), 0);
  EXPECT_FALSE(cache.IsDuplicateSet(key, 100));
}

// ============================================================================
// LRU Eviction Tests
// ============================================================================

TEST(StateCacheTest, LRUEviction_RemovesOldest) {
  StateCache cache(3);  // Max 3 entries

  StateKey key1("ctx1", "item1");
  StateKey key2("ctx1", "item2");
  StateKey key3("ctx1", "item3");
  StateKey key4("ctx1", "item4");

  cache.UpdateScore(key1, 100);
  cache.UpdateScore(key2, 200);
  cache.UpdateScore(key3, 300);

  EXPECT_EQ(cache.Size(), 3);

  // Adding key4 should evict key1 (oldest/LRU)
  cache.UpdateScore(key4, 400);

  EXPECT_EQ(cache.Size(), 3);

  // key1 should be evicted (LRU)
  EXPECT_FALSE(cache.IsDuplicateSet(key1, 100));

  // key2, key3, key4 should still exist
  EXPECT_TRUE(cache.IsDuplicateSet(key2, 200));
  EXPECT_TRUE(cache.IsDuplicateSet(key3, 300));
  EXPECT_TRUE(cache.IsDuplicateSet(key4, 400));
}

TEST(StateCacheTest, LRUEviction_AccessRefreshesOrder) {
  StateCache cache(3);  // Max 3 entries

  StateKey key1("ctx1", "item1");
  StateKey key2("ctx1", "item2");
  StateKey key3("ctx1", "item3");
  StateKey key4("ctx1", "item4");

  cache.UpdateScore(key1, 100);
  cache.UpdateScore(key2, 200);
  cache.UpdateScore(key3, 300);

  // Access key1 to refresh its position (UpdateScore moves to front)
  cache.UpdateScore(key1, 100);

  // Now key2 is the LRU. Adding key4 should evict key2.
  cache.UpdateScore(key4, 400);

  EXPECT_EQ(cache.Size(), 3);

  // key2 should be evicted (LRU after key1 was refreshed)
  EXPECT_FALSE(cache.IsDuplicateSet(key2, 200));

  // key1, key3, key4 should still exist
  EXPECT_TRUE(cache.IsDuplicateSet(key1, 100));
  EXPECT_TRUE(cache.IsDuplicateSet(key3, 300));
  EXPECT_TRUE(cache.IsDuplicateSet(key4, 400));
}

TEST(StateCacheTest, LRUEviction_MarkDeletedRefreshesOrder) {
  StateCache cache(3);

  StateKey key1("ctx1", "item1");
  StateKey key2("ctx1", "item2");
  StateKey key3("ctx1", "item3");
  StateKey key4("ctx1", "item4");

  cache.UpdateScore(key1, 100);
  cache.UpdateScore(key2, 200);
  cache.UpdateScore(key3, 300);

  // MarkDeleted on key1 should refresh it
  cache.MarkDeleted(key1);

  // Adding key4 should evict key2 (now LRU)
  cache.UpdateScore(key4, 400);

  EXPECT_FALSE(cache.IsDuplicateSet(key2, 200));
  EXPECT_TRUE(cache.IsDuplicateDel(key1));
  EXPECT_TRUE(cache.IsDuplicateSet(key3, 300));
  EXPECT_TRUE(cache.IsDuplicateSet(key4, 400));
}

TEST(StateCacheTest, Statistics) {
  StateCache cache(100);
  StateKey key("ctx1", "item1");

  cache.UpdateScore(key, 100);

  // Duplicate hit
  EXPECT_TRUE(cache.IsDuplicateSet(key, 100));
  // Miss (different score)
  EXPECT_FALSE(cache.IsDuplicateSet(key, 200));

  auto stats = cache.GetStatistics();
  EXPECT_EQ(stats.size, 1);
  EXPECT_EQ(stats.max_size, 100);
  EXPECT_EQ(stats.total_hits, 1);
  EXPECT_EQ(stats.total_misses, 1);
}

}  // namespace
}  // namespace nvecd::events
