/**
 * @file co_occurrence_index_test.cpp
 * @brief Unit tests for CoOccurrenceIndex
 */

#include "events/co_occurrence_index.h"

#include <gtest/gtest.h>

#include <thread>
#include <vector>

namespace nvecd::events {
namespace {

// Helper to create events
std::vector<Event> MakeEvents(
    const std::vector<std::tuple<std::string, int, uint64_t>>& data) {
  std::vector<Event> events;
  for (const auto& [id, score, timestamp] : data) {
    events.emplace_back(id, score, timestamp);
  }
  return events;
}

// ============================================================================
// Basic Operations
// ============================================================================

TEST(CoOccurrenceIndexTest, ConstructEmpty) {
  CoOccurrenceIndex index;
  EXPECT_EQ(index.GetItemCount(), 0);
  EXPECT_TRUE(index.GetAllItems().empty());
}

TEST(CoOccurrenceIndexTest, UpdateFromEmptyEvents) {
  CoOccurrenceIndex index;
  std::vector<Event> events;

  index.UpdateFromEvents("ctx1", events);

  EXPECT_EQ(index.GetItemCount(), 0);
}

TEST(CoOccurrenceIndexTest, UpdateFromSingleEvent) {
  CoOccurrenceIndex index;
  auto events = MakeEvents({{"item1", 10, 1000}});

  index.UpdateFromEvents("ctx1", events);

  // Single event has no co-occurrences
  EXPECT_EQ(index.GetItemCount(), 0);
}

TEST(CoOccurrenceIndexTest, UpdateFromTwoEvents) {
  CoOccurrenceIndex index;
  auto events = MakeEvents({{"item1", 10, 1000}, {"item2", 20, 1001}});

  index.UpdateFromEvents("ctx1", events);

  EXPECT_EQ(index.GetItemCount(), 2);

  // Score should be 10 * 20 = 200
  float score = index.GetScore("item1", "item2");
  EXPECT_FLOAT_EQ(score, 200.0f);

  // Symmetric
  EXPECT_FLOAT_EQ(index.GetScore("item2", "item1"), 200.0f);
}

TEST(CoOccurrenceIndexTest, UpdateFromMultipleEvents) {
  CoOccurrenceIndex index;
  auto events = MakeEvents({
      {"item1", 10, 1000},
      {"item2", 20, 1001},
      {"item3", 15, 1002},
  });

  index.UpdateFromEvents("ctx1", events);

  EXPECT_EQ(index.GetItemCount(), 3);

  // item1-item2: 10 * 20 = 200
  EXPECT_FLOAT_EQ(index.GetScore("item1", "item2"), 200.0f);

  // item1-item3: 10 * 15 = 150
  EXPECT_FLOAT_EQ(index.GetScore("item1", "item3"), 150.0f);

  // item2-item3: 20 * 15 = 300
  EXPECT_FLOAT_EQ(index.GetScore("item2", "item3"), 300.0f);
}

TEST(CoOccurrenceIndexTest, AccumulateScores) {
  CoOccurrenceIndex index;

  // First context
  auto events1 = MakeEvents({{"item1", 10, 1000}, {"item2", 20, 1001}});
  index.UpdateFromEvents("ctx1", events1);

  // item1-item2: 200
  EXPECT_FLOAT_EQ(index.GetScore("item1", "item2"), 200.0f);

  // Second context with same items
  auto events2 = MakeEvents({{"item1", 5, 2000}, {"item2", 10, 2001}});
  index.UpdateFromEvents("ctx2", events2);

  // item1-item2: 200 + (5 * 10) = 250
  EXPECT_FLOAT_EQ(index.GetScore("item1", "item2"), 250.0f);
}

// ============================================================================
// GetSimilar Tests
// ============================================================================

TEST(CoOccurrenceIndexTest, GetSimilarNoCoOccurrences) {
  CoOccurrenceIndex index;
  auto similar = index.GetSimilar("item1", 10);
  EXPECT_TRUE(similar.empty());
}

TEST(CoOccurrenceIndexTest, GetSimilarSinglePair) {
  CoOccurrenceIndex index;
  auto events = MakeEvents({{"item1", 10, 1000}, {"item2", 20, 1001}});
  index.UpdateFromEvents("ctx1", events);

  auto similar = index.GetSimilar("item1", 10);
  ASSERT_EQ(similar.size(), 1);
  EXPECT_EQ(similar[0].first, "item2");
  EXPECT_FLOAT_EQ(similar[0].second, 200.0f);
}

TEST(CoOccurrenceIndexTest, GetSimilarMultiplePairsSorted) {
  CoOccurrenceIndex index;
  auto events = MakeEvents({
      {"item1", 10, 1000},
      {"item2", 5, 1001},   // score: 50
      {"item3", 20, 1002},  // score: 200
      {"item4", 15, 1003},  // score: 150
  });
  index.UpdateFromEvents("ctx1", events);

  auto similar = index.GetSimilar("item1", 10);
  ASSERT_EQ(similar.size(), 3);

  // Should be sorted by score descending
  EXPECT_EQ(similar[0].first, "item3");
  EXPECT_FLOAT_EQ(similar[0].second, 200.0f);

  EXPECT_EQ(similar[1].first, "item4");
  EXPECT_FLOAT_EQ(similar[1].second, 150.0f);

  EXPECT_EQ(similar[2].first, "item2");
  EXPECT_FLOAT_EQ(similar[2].second, 50.0f);
}

TEST(CoOccurrenceIndexTest, GetSimilarTopK) {
  CoOccurrenceIndex index;
  auto events = MakeEvents({
      {"item1", 10, 1000}, {"item2", 5, 1001},  {"item3", 20, 1002},
      {"item4", 15, 1003}, {"item5", 25, 1004},
  });
  index.UpdateFromEvents("ctx1", events);

  auto similar = index.GetSimilar("item1", 2);
  ASSERT_EQ(similar.size(), 2);

  // Top 2 should be item5 (250) and item3 (200)
  EXPECT_EQ(similar[0].first, "item5");
  EXPECT_FLOAT_EQ(similar[0].second, 250.0f);

  EXPECT_EQ(similar[1].first, "item3");
  EXPECT_FLOAT_EQ(similar[1].second, 200.0f);
}

TEST(CoOccurrenceIndexTest, GetSimilarZeroTopK) {
  CoOccurrenceIndex index;
  auto events = MakeEvents({{"item1", 10, 1000}, {"item2", 20, 1001}});
  index.UpdateFromEvents("ctx1", events);

  auto similar = index.GetSimilar("item1", 0);
  EXPECT_TRUE(similar.empty());
}

TEST(CoOccurrenceIndexTest, GetSimilarNegativeTopK) {
  CoOccurrenceIndex index;
  auto events = MakeEvents({{"item1", 10, 1000}, {"item2", 20, 1001}});
  index.UpdateFromEvents("ctx1", events);

  auto similar = index.GetSimilar("item1", -1);
  EXPECT_TRUE(similar.empty());
}

// ============================================================================
// GetScore Tests
// ============================================================================

TEST(CoOccurrenceIndexTest, GetScoreNonexistent) {
  CoOccurrenceIndex index;
  EXPECT_FLOAT_EQ(index.GetScore("item1", "item2"), 0.0f);
}

TEST(CoOccurrenceIndexTest, GetScoreSymmetric) {
  CoOccurrenceIndex index;
  auto events = MakeEvents({{"item1", 10, 1000}, {"item2", 20, 1001}});
  index.UpdateFromEvents("ctx1", events);

  float score12 = index.GetScore("item1", "item2");
  float score21 = index.GetScore("item2", "item1");

  EXPECT_FLOAT_EQ(score12, score21);
  EXPECT_FLOAT_EQ(score12, 200.0f);
}

TEST(CoOccurrenceIndexTest, GetScoreSelfPair) {
  CoOccurrenceIndex index;
  auto events = MakeEvents({{"item1", 10, 1000}, {"item1", 20, 1001}});
  index.UpdateFromEvents("ctx1", events);

  // Self-pairs should be skipped
  EXPECT_FLOAT_EQ(index.GetScore("item1", "item1"), 0.0f);
}

// ============================================================================
// Decay Tests
// ============================================================================

TEST(CoOccurrenceIndexTest, ApplyDecay) {
  CoOccurrenceIndex index;
  auto events = MakeEvents({{"item1", 10, 1000}, {"item2", 20, 1001}});
  index.UpdateFromEvents("ctx1", events);

  EXPECT_FLOAT_EQ(index.GetScore("item1", "item2"), 200.0f);

  index.ApplyDecay(0.5);

  EXPECT_FLOAT_EQ(index.GetScore("item1", "item2"), 100.0f);
}

TEST(CoOccurrenceIndexTest, ApplyDecayMultipleTimes) {
  CoOccurrenceIndex index;
  auto events = MakeEvents({{"item1", 10, 1000}, {"item2", 20, 1001}});
  index.UpdateFromEvents("ctx1", events);

  index.ApplyDecay(0.9);
  index.ApplyDecay(0.9);

  // 200 * 0.9 * 0.9 = 162
  EXPECT_FLOAT_EQ(index.GetScore("item1", "item2"), 162.0f);
}

TEST(CoOccurrenceIndexTest, ApplyDecayInvalidAlpha) {
  CoOccurrenceIndex index;
  auto events = MakeEvents({{"item1", 10, 1000}, {"item2", 20, 1001}});
  index.UpdateFromEvents("ctx1", events);

  float original_score = index.GetScore("item1", "item2");

  // Invalid alpha values should be ignored
  index.ApplyDecay(0.0);
  EXPECT_FLOAT_EQ(index.GetScore("item1", "item2"), original_score);

  index.ApplyDecay(-0.5);
  EXPECT_FLOAT_EQ(index.GetScore("item1", "item2"), original_score);

  index.ApplyDecay(1.5);
  EXPECT_FLOAT_EQ(index.GetScore("item1", "item2"), original_score);
}

// ============================================================================
// Clear Tests
// ============================================================================

TEST(CoOccurrenceIndexTest, ClearEmpty) {
  CoOccurrenceIndex index;
  index.Clear();

  EXPECT_EQ(index.GetItemCount(), 0);
}

TEST(CoOccurrenceIndexTest, ClearWithData) {
  CoOccurrenceIndex index;
  auto events = MakeEvents({{"item1", 10, 1000}, {"item2", 20, 1001}});
  index.UpdateFromEvents("ctx1", events);

  EXPECT_EQ(index.GetItemCount(), 2);

  index.Clear();

  EXPECT_EQ(index.GetItemCount(), 0);
  EXPECT_FLOAT_EQ(index.GetScore("item1", "item2"), 0.0f);
  EXPECT_TRUE(index.GetAllItems().empty());
}

TEST(CoOccurrenceIndexTest, ReuseAfterClear) {
  CoOccurrenceIndex index;
  auto events1 = MakeEvents({{"item1", 10, 1000}, {"item2", 20, 1001}});
  index.UpdateFromEvents("ctx1", events1);

  index.Clear();

  auto events2 = MakeEvents({{"item3", 5, 2000}, {"item4", 15, 2001}});
  index.UpdateFromEvents("ctx2", events2);

  EXPECT_EQ(index.GetItemCount(), 2);
  EXPECT_FLOAT_EQ(index.GetScore("item3", "item4"), 75.0f);
  EXPECT_FLOAT_EQ(index.GetScore("item1", "item2"), 0.0f);
}

// ============================================================================
// GetAllItems Tests
// ============================================================================

TEST(CoOccurrenceIndexTest, GetAllItems) {
  CoOccurrenceIndex index;
  auto events = MakeEvents({
      {"item1", 10, 1000},
      {"item2", 20, 1001},
      {"item3", 15, 1002},
  });
  index.UpdateFromEvents("ctx1", events);

  auto items = index.GetAllItems();
  EXPECT_EQ(items.size(), 3);

  std::sort(items.begin(), items.end());
  EXPECT_EQ(items[0], "item1");
  EXPECT_EQ(items[1], "item2");
  EXPECT_EQ(items[2], "item3");
}

// ============================================================================
// Edge Cases
// ============================================================================

TEST(CoOccurrenceIndexTest, NegativeScores) {
  CoOccurrenceIndex index;
  auto events = MakeEvents({{"item1", -10, 1000}, {"item2", 20, 1001}});
  index.UpdateFromEvents("ctx1", events);

  // -10 * 20 = -200
  EXPECT_FLOAT_EQ(index.GetScore("item1", "item2"), -200.0f);
}

TEST(CoOccurrenceIndexTest, ZeroScores) {
  CoOccurrenceIndex index;
  auto events = MakeEvents({{"item1", 0, 1000}, {"item2", 20, 1001}});
  index.UpdateFromEvents("ctx1", events);

  // 0 * 20 = 0
  EXPECT_FLOAT_EQ(index.GetScore("item1", "item2"), 0.0f);

  // GetSimilar should filter out zero scores
  auto similar = index.GetSimilar("item1", 10);
  EXPECT_TRUE(similar.empty());
}

TEST(CoOccurrenceIndexTest, LargeNumberOfEvents) {
  CoOccurrenceIndex index;
  std::vector<Event> events;
  for (int i = 0; i < 100; ++i) {
    events.emplace_back("item" + std::to_string(i), 10, 1000 + i);
  }

  index.UpdateFromEvents("ctx1", events);

  // 100 items, each pair co-occurs once
  EXPECT_EQ(index.GetItemCount(), 100);

  // Check a sample pair
  EXPECT_FLOAT_EQ(index.GetScore("item0", "item1"), 100.0f);
}

// ============================================================================
// Concurrency Tests
// ============================================================================

TEST(CoOccurrenceIndexTest, ConcurrentUpdates) {
  CoOccurrenceIndex index;

  constexpr int num_threads = 10;
  constexpr int updates_per_thread = 100;

  std::vector<std::thread> threads;
  for (int t = 0; t < num_threads; ++t) {
    threads.emplace_back([&index, t]() {
      for (int i = 0; i < updates_per_thread; ++i) {
        auto events = MakeEvents({
            {"item" + std::to_string(t), 10, 1000},
            {"common", 5, 1001},
        });
        index.UpdateFromEvents("ctx" + std::to_string(t * 100 + i), events);
      }
    });
  }

  for (auto& thread : threads) {
    thread.join();
  }

  // All threads should have updated "common" co-occurrences
  EXPECT_GT(index.GetItemCount(), 0);
}

TEST(CoOccurrenceIndexTest, ConcurrentReadsAndWrites) {
  CoOccurrenceIndex index;

  // Initialize with some data
  auto events = MakeEvents({{"item1", 10, 1000}, {"item2", 20, 1001}});
  index.UpdateFromEvents("ctx1", events);

  std::atomic<bool> stop{false};

  // Writer thread
  std::thread writer([&index, &stop]() {
    int counter = 0;
    while (!stop.load()) {
      auto events = MakeEvents({
          {"item1", 10, 1000},
          {"item" + std::to_string(counter++), 5, 1001},
      });
      index.UpdateFromEvents("ctx_writer", events);
      std::this_thread::sleep_for(std::chrono::microseconds(10));
    }
  });

  // Reader threads
  std::vector<std::thread> readers;
  for (int i = 0; i < 5; ++i) {
    readers.emplace_back([&index, &stop]() {
      while (!stop.load()) {
        auto similar = index.GetSimilar("item1", 10);
        index.GetScore("item1", "item2");
        index.GetItemCount();
      }
    });
  }

  // Run for a short time
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  stop.store(true);

  writer.join();
  for (auto& reader : readers) {
    reader.join();
  }

  EXPECT_GT(index.GetItemCount(), 0);
}

}  // namespace
}  // namespace nvecd::events
