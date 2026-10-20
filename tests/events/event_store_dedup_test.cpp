/**
 * @file event_store_dedup_test.cpp
 * @brief Unit tests for EventStore deduplication
 */

#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <vector>

#include "events/event_store.h"

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
  EXPECT_EQ(events[0].item_id, "item1");
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

// Recovery bypasses normal ingestion, but must still reconstruct SET/DEL state
// so replaying a record already represented by the snapshot is idempotent.
TEST_F(EventStoreDeduplicationTest, RestoreSeedsStateCacheForSetAndDelete) {
  EventStore store(config_);
  ASSERT_TRUE(store.RestoreEvent("ctx", Event("item", 42, 100, EventType::SET)));

  auto set_replay = store.AddEventAndGetPrior("ctx", "item", 42, EventType::SET, 101);
  ASSERT_TRUE(set_replay);
  EXPECT_TRUE(set_replay->deduped);

  ASSERT_TRUE(store.RestoreEvent("ctx", Event("item", 0, 102, EventType::DEL)));
  auto del_replay = store.AddEventAndGetPrior("ctx", "item", 0, EventType::DEL, 103);
  ASSERT_TRUE(del_replay);
  EXPECT_TRUE(del_replay->deduped);
}

TEST_F(EventStoreDeduplicationTest, RestoreSeedsAddDedupWindow) {
  EventStore store(config_);
  ASSERT_TRUE(store.RestoreEvent("ctx", Event("item", 42, 1000, EventType::ADD)));

  auto retry = store.AddEventAndGetPrior("ctx", "item", 42, EventType::ADD, 1030);
  ASSERT_TRUE(retry);
  EXPECT_TRUE(retry->deduped);
  EXPECT_EQ(store.GetEvents("ctx").size(), 1U);

  auto later = store.AddEventAndGetPrior("ctx", "item", 42, EventType::ADD, 1100);
  ASSERT_TRUE(later);
  EXPECT_FALSE(later->deduped);
}

TEST_F(EventStoreDeduplicationTest, SecondDelAfterReAddIsApplied) {
  EventStore store(config_);
  ASSERT_TRUE(store.AddEvent("ctx", "item", 80, EventType::ADD, 1000));
  auto first_del = store.AddEventAndGetPrior("ctx", "item", 0, EventType::DEL, 1001);
  ASSERT_TRUE(first_del);
  EXPECT_FALSE(first_del->deduped);

  auto repeat_del = store.AddEventAndGetPrior("ctx", "item", 0, EventType::DEL, 1002);
  ASSERT_TRUE(repeat_del);
  EXPECT_TRUE(repeat_del->deduped);

  auto re_add = store.AddEventAndGetPrior("ctx", "item", 80, EventType::ADD, 2000);
  ASSERT_TRUE(re_add);
  EXPECT_FALSE(re_add->deduped);

  auto prepared = store.PrepareEvent("ctx", "item", 0, EventType::DEL, 2001);
  ASSERT_TRUE(prepared);
  EXPECT_FALSE(prepared->deduped);
  auto second_del = store.AddEventAndGetPrior("ctx", "item", 0, EventType::DEL, 2001);
  ASSERT_TRUE(second_del);
  EXPECT_FALSE(second_del->deduped);
}

TEST_F(EventStoreDeduplicationTest, RestoredAddClearsDeletedMarker) {
  EventStore store(config_);
  ASSERT_TRUE(store.RestoreEvent("ctx", Event("item", 0, 1000, EventType::DEL)));
  ASSERT_TRUE(store.RestoreEvent("ctx", Event("item", 80, 2000, EventType::ADD)));

  auto del = store.AddEventAndGetPrior("ctx", "item", 0, EventType::DEL, 2001);
  ASSERT_TRUE(del);
  EXPECT_FALSE(del->deduped);
}

// A logged record was accepted exactly once live, so replay applies it even
// when the caches already hold an identical entry, and returns the prior buffer.
TEST_F(EventStoreDeduplicationTest, ReplayAppliesRecordsRegardlessOfCacheContents) {
  EventStore store(config_);
  ASSERT_TRUE(store.AddEvent("ctx", "item", 42, EventType::ADD, 1000));

  auto replayed = store.ReplayEvent("ctx", Event("item", 42, 1010, EventType::ADD));
  ASSERT_TRUE(replayed);
  EXPECT_FALSE(replayed->deduped);
  EXPECT_EQ(replayed->prior_events.size(), 1U);
  EXPECT_EQ(replayed->stored_event.timestamp, 1010U);
  EXPECT_EQ(store.GetEvents("ctx").size(), 2U);

  auto retry = store.AddEventAndGetPrior("ctx", "item", 42, EventType::ADD, 1020);
  ASSERT_TRUE(retry);
  EXPECT_TRUE(retry->deduped);
}

TEST_F(EventStoreDeduplicationTest, ReplayRejectsInvalidEvents) {
  EventStore store(config_);
  EXPECT_FALSE(store.ReplayEvent("ctx", Event("item", 101, 1000, EventType::ADD)));
  EXPECT_EQ(store.GetEvents("ctx").size(), 0U);
}

// The duplicate check and cache update must be a single transaction. Under the
// former check-then-insert flow multiple concurrent ADDs could all pass the
// read-side check before any writer recorded the key.
TEST_F(EventStoreDeduplicationTest, ConcurrentDuplicateAddsAreStoredExactlyOnce) {
  EventStore store(config_);
  constexpr int kThreads = 64;
  std::atomic<int> accepted{0};
  std::vector<std::thread> workers;
  workers.reserve(kThreads);

  for (int i = 0; i < kThreads; ++i) {
    workers.emplace_back([&]() {
      auto result = store.AddEventAndGetPrior("ctx", "item", 42, EventType::ADD, 1000);
      ASSERT_TRUE(result);
      if (!result->deduped) {
        accepted.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }
  for (auto& worker : workers) {
    worker.join();
  }

  EXPECT_EQ(accepted.load(), 1);
  const auto stats = store.GetStatistics();
  EXPECT_EQ(stats.total_events, kThreads);
  EXPECT_EQ(stats.deduped_events, kThreads - 1);
  EXPECT_EQ(stats.stored_events, 1U);
}

TEST_F(EventStoreDeduplicationTest, ConcurrentSetsLeaveCacheMatchingTheFinalBufferedEvent) {
  constexpr size_t kWarmupEvents = 2048;
  constexpr size_t kWarmupIdSize = 1024;
  config_.ctx_buffer_size = static_cast<uint32_t>(kWarmupEvents + 8);
  EventStore store(config_);

  // A large prior buffer widens any window between the cache update and the append.
  const std::string padding(kWarmupIdSize, 'x');
  for (size_t i = 0; i < kWarmupEvents; ++i) {
    const std::string id = "warmup" + std::to_string(i) + padding;
    ASSERT_TRUE(store.RestoreEvent("ctx", Event(id, 1, static_cast<uint64_t>(i + 1), EventType::ADD)));
  }

  std::atomic<int> ready{0};
  std::atomic<bool> start{false};
  std::atomic<int> failures{0};
  std::vector<std::thread> workers;
  workers.reserve(8);
  for (int i = 0; i < 8; ++i) {
    workers.emplace_back([&, i]() {
      ready.fetch_add(1, std::memory_order_release);
      while (!start.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      for (int round = 0; round < 8; ++round) {
        const int score = 1 + ((i * 17 + round * 31) % 100);
        auto result = store.AddEventAndGetPrior("ctx", "shared-state", score, EventType::SET,
                                                100000 + static_cast<uint64_t>(i * 8 + round));
        if (!result) {
          failures.fetch_add(1, std::memory_order_relaxed);
        }
      }
    });
  }
  while (ready.load(std::memory_order_acquire) != 8) {
    std::this_thread::yield();
  }
  start.store(true, std::memory_order_release);
  for (auto& worker : workers) {
    worker.join();
  }
  ASSERT_EQ(failures.load(), 0);

  const auto events = store.GetEvents("ctx");
  ASSERT_GT(events.size(), kWarmupEvents);
  ASSERT_EQ(events.back().item_id, "shared-state");
  const auto final_score = events.back().score;
  const auto size_before_replay = events.size();

  auto replay = store.AddEventAndGetPrior("ctx", "shared-state", final_score, EventType::SET, 200000);
  ASSERT_TRUE(replay);
  EXPECT_TRUE(replay->deduped);
  EXPECT_EQ(store.GetEvents("ctx").size(), size_before_replay);
}

}  // namespace nvecd::events
