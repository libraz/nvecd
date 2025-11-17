/**
 * @file event_store_test.cpp
 * @brief Unit tests for EventStore
 */

#include "events/event_store.h"

#include <gtest/gtest.h>

#include <thread>
#include <vector>

namespace nvecd::events {
namespace {

// Helper to create default config
config::EventsConfig MakeConfig(uint32_t buffer_size = 50) {
  config::EventsConfig config;
  config.ctx_buffer_size = buffer_size;
  config.decay_interval_sec = 3600;
  config.decay_alpha = 0.99;
  return config;
}

// ============================================================================
// Basic Operations
// ============================================================================

TEST(EventStoreTest, ConstructEmpty) {
  auto config = MakeConfig();
  EventStore store(config);

  EXPECT_EQ(store.GetContextCount(), 0);
  EXPECT_EQ(store.GetTotalEventCount(), 0);
  EXPECT_TRUE(store.GetAllContexts().empty());
}

TEST(EventStoreTest, AddSingleEvent) {
  auto config = MakeConfig();
  EventStore store(config);

  auto result = store.AddEvent("user1", "item1", 10);
  ASSERT_TRUE(result.has_value()) << result.error().message();

  EXPECT_EQ(store.GetContextCount(), 1);
  EXPECT_EQ(store.GetTotalEventCount(), 1);

  auto events = store.GetEvents("user1");
  ASSERT_EQ(events.size(), 1);
  EXPECT_EQ(events[0].id, "item1");
  EXPECT_EQ(events[0].score, 10);
  EXPECT_GT(events[0].timestamp, 0);
}

TEST(EventStoreTest, AddMultipleEventsToSameContext) {
  auto config = MakeConfig();
  EventStore store(config);

  ASSERT_TRUE(store.AddEvent("user1", "item1", 10).has_value());
  ASSERT_TRUE(store.AddEvent("user1", "item2", 20).has_value());
  ASSERT_TRUE(store.AddEvent("user1", "item3", 30).has_value());

  EXPECT_EQ(store.GetContextCount(), 1);
  EXPECT_EQ(store.GetTotalEventCount(), 3);

  auto events = store.GetEvents("user1");
  ASSERT_EQ(events.size(), 3);
  EXPECT_EQ(events[0].id, "item1");
  EXPECT_EQ(events[1].id, "item2");
  EXPECT_EQ(events[2].id, "item3");
}

TEST(EventStoreTest, AddEventsToMultipleContexts) {
  auto config = MakeConfig();
  EventStore store(config);

  ASSERT_TRUE(store.AddEvent("user1", "item1", 10).has_value());
  ASSERT_TRUE(store.AddEvent("user2", "item2", 20).has_value());
  ASSERT_TRUE(store.AddEvent("user3", "item3", 30).has_value());

  EXPECT_EQ(store.GetContextCount(), 3);
  EXPECT_EQ(store.GetTotalEventCount(), 3);

  auto contexts = store.GetAllContexts();
  EXPECT_EQ(contexts.size(), 3);

  auto events1 = store.GetEvents("user1");
  ASSERT_EQ(events1.size(), 1);
  EXPECT_EQ(events1[0].id, "item1");

  auto events2 = store.GetEvents("user2");
  ASSERT_EQ(events2.size(), 1);
  EXPECT_EQ(events2[0].id, "item2");
}

// ============================================================================
// Ring Buffer Behavior
// ============================================================================

TEST(EventStoreTest, RingBufferOverwrite) {
  auto config = MakeConfig(3);  // Small buffer
  EventStore store(config);

  ASSERT_TRUE(store.AddEvent("user1", "item1", 10).has_value());
  ASSERT_TRUE(store.AddEvent("user1", "item2", 20).has_value());
  ASSERT_TRUE(store.AddEvent("user1", "item3", 30).has_value());
  ASSERT_TRUE(store.AddEvent("user1", "item4", 40).has_value());  // Overwrite item1

  EXPECT_EQ(store.GetContextCount(), 1);
  EXPECT_EQ(store.GetTotalEventCount(), 4);  // Total includes overwritten

  auto events = store.GetEvents("user1");
  ASSERT_EQ(events.size(), 3);
  EXPECT_EQ(events[0].id, "item2");
  EXPECT_EQ(events[1].id, "item3");
  EXPECT_EQ(events[2].id, "item4");
}

TEST(EventStoreTest, MultipleOverwrites) {
  auto config = MakeConfig(2);  // Very small buffer
  EventStore store(config);

  for (int i = 1; i <= 10; ++i) {
    ASSERT_TRUE(store.AddEvent("user1", "item" + std::to_string(i), i * 10)
                    .has_value());
  }

  EXPECT_EQ(store.GetTotalEventCount(), 10);

  auto events = store.GetEvents("user1");
  ASSERT_EQ(events.size(), 2);
  EXPECT_EQ(events[0].id, "item9");
  EXPECT_EQ(events[1].id, "item10");
}

// ============================================================================
// Validation
// ============================================================================

TEST(EventStoreTest, EmptyContext) {
  auto config = MakeConfig();
  EventStore store(config);

  auto result = store.AddEvent("", "item1", 10);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), utils::ErrorCode::kInvalidArgument);
  EXPECT_NE(result.error().message().find("Context"), std::string::npos);

  EXPECT_EQ(store.GetContextCount(), 0);
  EXPECT_EQ(store.GetTotalEventCount(), 0);
}

TEST(EventStoreTest, EmptyId) {
  auto config = MakeConfig();
  EventStore store(config);

  auto result = store.AddEvent("user1", "", 10);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), utils::ErrorCode::kInvalidArgument);
  EXPECT_NE(result.error().message().find("ID"), std::string::npos);

  EXPECT_EQ(store.GetContextCount(), 0);
  EXPECT_EQ(store.GetTotalEventCount(), 0);
}

TEST(EventStoreTest, NegativeScore) {
  auto config = MakeConfig();
  EventStore store(config);

  // Negative scores should be allowed
  auto result = store.AddEvent("user1", "item1", -5);
  ASSERT_TRUE(result.has_value());

  auto events = store.GetEvents("user1");
  ASSERT_EQ(events.size(), 1);
  EXPECT_EQ(events[0].score, -5);
}

TEST(EventStoreTest, ZeroScore) {
  auto config = MakeConfig();
  EventStore store(config);

  // Zero scores should be allowed
  auto result = store.AddEvent("user1", "item1", 0);
  ASSERT_TRUE(result.has_value());

  auto events = store.GetEvents("user1");
  ASSERT_EQ(events.size(), 1);
  EXPECT_EQ(events[0].score, 0);
}

// ============================================================================
// Query Operations
// ============================================================================

TEST(EventStoreTest, GetEventsNonexistentContext) {
  auto config = MakeConfig();
  EventStore store(config);

  auto events = store.GetEvents("nonexistent");
  EXPECT_TRUE(events.empty());
}

TEST(EventStoreTest, GetAllContexts) {
  auto config = MakeConfig();
  EventStore store(config);

  ASSERT_TRUE(store.AddEvent("user1", "item1", 10).has_value());
  ASSERT_TRUE(store.AddEvent("user2", "item2", 20).has_value());
  ASSERT_TRUE(store.AddEvent("user3", "item3", 30).has_value());

  auto contexts = store.GetAllContexts();
  ASSERT_EQ(contexts.size(), 3);

  // Check that all contexts are present (order not guaranteed)
  std::vector<std::string> sorted_contexts = contexts;
  std::sort(sorted_contexts.begin(), sorted_contexts.end());
  EXPECT_EQ(sorted_contexts[0], "user1");
  EXPECT_EQ(sorted_contexts[1], "user2");
  EXPECT_EQ(sorted_contexts[2], "user3");
}

// ============================================================================
// Clear Operations
// ============================================================================

TEST(EventStoreTest, ClearEmpty) {
  auto config = MakeConfig();
  EventStore store(config);

  store.Clear();

  EXPECT_EQ(store.GetContextCount(), 0);
  EXPECT_EQ(store.GetTotalEventCount(), 0);
}

TEST(EventStoreTest, ClearWithData) {
  auto config = MakeConfig();
  EventStore store(config);

  ASSERT_TRUE(store.AddEvent("user1", "item1", 10).has_value());
  ASSERT_TRUE(store.AddEvent("user2", "item2", 20).has_value());

  EXPECT_EQ(store.GetContextCount(), 2);
  EXPECT_EQ(store.GetTotalEventCount(), 2);

  store.Clear();

  EXPECT_EQ(store.GetContextCount(), 0);
  EXPECT_EQ(store.GetTotalEventCount(), 0);
  EXPECT_TRUE(store.GetAllContexts().empty());
  EXPECT_TRUE(store.GetEvents("user1").empty());
}

TEST(EventStoreTest, ReuseAfterClear) {
  auto config = MakeConfig();
  EventStore store(config);

  ASSERT_TRUE(store.AddEvent("user1", "item1", 10).has_value());
  store.Clear();

  ASSERT_TRUE(store.AddEvent("user2", "item2", 20).has_value());

  EXPECT_EQ(store.GetContextCount(), 1);
  EXPECT_EQ(store.GetTotalEventCount(), 1);

  auto events = store.GetEvents("user2");
  ASSERT_EQ(events.size(), 1);
  EXPECT_EQ(events[0].id, "item2");
}

// ============================================================================
// Concurrency Tests
// ============================================================================

TEST(EventStoreTest, ConcurrentWrites) {
  auto config = MakeConfig();
  EventStore store(config);

  constexpr int num_threads = 10;
  constexpr int events_per_thread = 100;

  std::vector<std::thread> threads;
  for (int t = 0; t < num_threads; ++t) {
    threads.emplace_back([&store, t]() {
      std::string ctx = "user" + std::to_string(t);
      for (int i = 0; i < events_per_thread; ++i) {
        auto result =
            store.AddEvent(ctx, "item" + std::to_string(i), i);
        EXPECT_TRUE(result.has_value());
      }
    });
  }

  for (auto& thread : threads) {
    thread.join();
  }

  EXPECT_EQ(store.GetContextCount(), num_threads);
  EXPECT_EQ(store.GetTotalEventCount(), num_threads * events_per_thread);
}

TEST(EventStoreTest, ConcurrentReadsAndWrites) {
  auto config = MakeConfig();
  EventStore store(config);

  // Add initial data
  for (int i = 0; i < 100; ++i) {
    ASSERT_TRUE(store.AddEvent("user1", "item" + std::to_string(i), i)
                    .has_value());
  }

  std::atomic<bool> stop{false};
  std::atomic<int> read_count{0};

  // Writer thread
  std::thread writer([&store, &stop]() {
    int counter = 100;
    while (!stop.load()) {
      store.AddEvent("user1", "item" + std::to_string(counter), counter);
      ++counter;
      std::this_thread::sleep_for(std::chrono::microseconds(10));
    }
  });

  // Reader threads
  std::vector<std::thread> readers;
  for (int i = 0; i < 5; ++i) {
    readers.emplace_back([&store, &stop, &read_count]() {
      while (!stop.load()) {
        auto events = store.GetEvents("user1");
        EXPECT_GT(events.size(), 0);
        read_count.fetch_add(1);
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

  EXPECT_GT(read_count.load(), 0);
}

// ============================================================================
// Edge Cases
// ============================================================================

TEST(EventStoreTest, VeryLongStrings) {
  auto config = MakeConfig();
  EventStore store(config);

  std::string long_ctx(10000, 'a');
  std::string long_id(10000, 'b');

  auto result = store.AddEvent(long_ctx, long_id, 10);
  ASSERT_TRUE(result.has_value());

  auto events = store.GetEvents(long_ctx);
  ASSERT_EQ(events.size(), 1);
  EXPECT_EQ(events[0].id, long_id);
}

TEST(EventStoreTest, SpecialCharacters) {
  auto config = MakeConfig();
  EventStore store(config);

  std::string special_ctx = "user@#$%^&*()";
  std::string special_id = "item\n\t\r";

  auto result = store.AddEvent(special_ctx, special_id, 10);
  ASSERT_TRUE(result.has_value());

  auto events = store.GetEvents(special_ctx);
  ASSERT_EQ(events.size(), 1);
  EXPECT_EQ(events[0].id, special_id);
}

}  // namespace
}  // namespace nvecd::events
