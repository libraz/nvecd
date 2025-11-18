/**
 * @file event_store_dedup_test.cpp
 * @brief Unit tests for EventStore deduplication
 */

#include "events/event_store.h"

#include <gtest/gtest.h>

#include <thread>

namespace nvecd::events {

class EventStoreDeduplicationTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // Enable deduplication
    config_.ctx_buffer_size = 50;
    config_.dedup_window_sec = 60;
    config_.dedup_cache_size = 1000;
  }

  config::EventsConfig config_;
};

TEST_F(EventStoreDeduplicationTest, DuplicateEventIgnored) {
  EventStore store(config_);

  // Add event
  auto result = store.AddEvent("user1", "item1", 95);
  ASSERT_TRUE(result);

  auto stats = store.GetStatistics();
  EXPECT_EQ(stats.total_events, 1);
  EXPECT_EQ(stats.deduped_events, 0);
  EXPECT_EQ(stats.stored_events, 1);

  // Immediately add same event (within 60 sec window)
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  result = store.AddEvent("user1", "item1", 95);
  ASSERT_TRUE(result);  // Still returns success

  stats = store.GetStatistics();
  EXPECT_EQ(stats.total_events, 2);
  EXPECT_EQ(stats.deduped_events, 1);  // One duplicate
  EXPECT_EQ(stats.stored_events, 1);   // Still only 1 stored

  // Verify only one event in context
  auto events = store.GetEvents("user1");
  EXPECT_EQ(events.size(), 1);
  EXPECT_EQ(events[0].id, "item1");
  EXPECT_EQ(events[0].score, 95);
}

TEST_F(EventStoreDeduplicationTest, DifferentScoreNotDuplicate) {
  EventStore store(config_);

  auto result = store.AddEvent("user1", "item1", 95);
  ASSERT_TRUE(result);

  // Same ctx/id but different score - should not be duplicate
  result = store.AddEvent("user1", "item1", 90);
  ASSERT_TRUE(result);

  auto stats = store.GetStatistics();
  EXPECT_EQ(stats.total_events, 2);
  EXPECT_EQ(stats.deduped_events, 0);  // No duplicates
  EXPECT_EQ(stats.stored_events, 2);   // Both stored

  auto events = store.GetEvents("user1");
  EXPECT_EQ(events.size(), 2);
}

TEST_F(EventStoreDeduplicationTest, DifferentContextNotDuplicate) {
  EventStore store(config_);

  auto result = store.AddEvent("user1", "item1", 95);
  ASSERT_TRUE(result);

  // Different context - should not be duplicate
  result = store.AddEvent("user2", "item1", 95);
  ASSERT_TRUE(result);

  auto stats = store.GetStatistics();
  EXPECT_EQ(stats.total_events, 2);
  EXPECT_EQ(stats.deduped_events, 0);
  EXPECT_EQ(stats.stored_events, 2);

  EXPECT_EQ(store.GetContextCount(), 2);
}

TEST_F(EventStoreDeduplicationTest, MultipleRepeatedEvents) {
  EventStore store(config_);

  // Simulate bug: same event sent 100 times
  for (int i = 0; i < 100; ++i) {
    auto result = store.AddEvent("user1", "item1", 95);
    ASSERT_TRUE(result);
  }

  auto stats = store.GetStatistics();
  EXPECT_EQ(stats.total_events, 100);
  EXPECT_EQ(stats.deduped_events, 99);  // 99 duplicates
  EXPECT_EQ(stats.stored_events, 1);    // Only 1 actually stored

  auto events = store.GetEvents("user1");
  EXPECT_EQ(events.size(), 1);
}

TEST_F(EventStoreDeduplicationTest, DeduplicationDisabled) {
  // Disable deduplication
  config_.dedup_window_sec = 0;
  EventStore store(config_);

  // Add same event twice
  auto result = store.AddEvent("user1", "item1", 95);
  ASSERT_TRUE(result);

  result = store.AddEvent("user1", "item1", 95);
  ASSERT_TRUE(result);

  auto stats = store.GetStatistics();
  EXPECT_EQ(stats.total_events, 2);
  EXPECT_EQ(stats.deduped_events, 0);  // No dedup
  EXPECT_EQ(stats.stored_events, 2);   // Both stored

  auto events = store.GetEvents("user1");
  EXPECT_EQ(events.size(), 2);
}

TEST_F(EventStoreDeduplicationTest, ClearResetsStats) {
  EventStore store(config_);

  // Add events with duplicates
  store.AddEvent("user1", "item1", 95);
  store.AddEvent("user1", "item1", 95);  // Duplicate

  auto stats = store.GetStatistics();
  EXPECT_EQ(stats.total_events, 2);
  EXPECT_EQ(stats.deduped_events, 1);

  // Clear
  store.Clear();

  stats = store.GetStatistics();
  EXPECT_EQ(stats.total_events, 0);
  EXPECT_EQ(stats.deduped_events, 0);
  EXPECT_EQ(stats.stored_events, 0);
  EXPECT_EQ(store.GetContextCount(), 0);
}

TEST_F(EventStoreDeduplicationTest, MixedDuplicateAndUnique) {
  EventStore store(config_);

  // Add mix of unique and duplicate events
  store.AddEvent("user1", "item1", 95);
  store.AddEvent("user1", "item2", 90);
  store.AddEvent("user1", "item1", 95);  // Duplicate
  store.AddEvent("user1", "item3", 85);
  store.AddEvent("user1", "item2", 90);  // Duplicate

  auto stats = store.GetStatistics();
  EXPECT_EQ(stats.total_events, 5);
  EXPECT_EQ(stats.deduped_events, 2);
  EXPECT_EQ(stats.stored_events, 3);  // 3 unique events

  auto events = store.GetEvents("user1");
  EXPECT_EQ(events.size(), 3);
}

}  // namespace nvecd::events
