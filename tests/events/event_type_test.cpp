/**
 * @file event_type_test.cpp
 * @brief Unit tests for ADD/SET/DEL event types
 */

#include "events/event_store.h"

#include <gtest/gtest.h>

#include <thread>

namespace nvecd::events {

class EventTypeTest : public ::testing::Test {
 protected:
  void SetUp() override {
    config_.ctx_buffer_size = 50;
    config_.dedup_window_sec = 60;
    config_.dedup_cache_size = 1000;
  }

  config::EventsConfig config_;
};

// ============================================================================
// ADD Type Tests
// ============================================================================

TEST_F(EventTypeTest, AddTypeBasic) {
  EventStore store(config_);

  auto result = store.AddEvent("user1", "item1", 100, EventType::ADD);
  ASSERT_TRUE(result);

  auto events = store.GetEvents("user1");
  ASSERT_EQ(events.size(), 1);
  EXPECT_EQ(events[0].id, "item1");
  EXPECT_EQ(events[0].score, 100);
  EXPECT_EQ(events[0].type, EventType::ADD);
}

TEST_F(EventTypeTest, AddTypeDuplicateWithinWindow) {
  EventStore store(config_);

  // First ADD
  ASSERT_TRUE(store.AddEvent("user1", "item1", 100, EventType::ADD));

  // Immediate duplicate (within 60 sec window)
  ASSERT_TRUE(store.AddEvent("user1", "item1", 100, EventType::ADD));

  auto stats = store.GetStatistics();
  EXPECT_EQ(stats.total_events, 2);
  EXPECT_EQ(stats.deduped_events, 1);
  EXPECT_EQ(stats.stored_events, 1);
}

TEST_F(EventTypeTest, AddTypeDifferentScoreNotDuplicate) {
  EventStore store(config_);

  ASSERT_TRUE(store.AddEvent("user1", "item1", 100, EventType::ADD));
  ASSERT_TRUE(store.AddEvent("user1", "item1", 90, EventType::ADD));

  auto stats = store.GetStatistics();
  EXPECT_EQ(stats.total_events, 2);
  EXPECT_EQ(stats.deduped_events, 0);
  EXPECT_EQ(stats.stored_events, 2);
}

// ============================================================================
// SET Type Tests
// ============================================================================

TEST_F(EventTypeTest, SetTypeBasic) {
  EventStore store(config_);

  auto result = store.AddEvent("user1", "like:item1", 100, EventType::SET);
  ASSERT_TRUE(result);

  auto events = store.GetEvents("user1");
  ASSERT_EQ(events.size(), 1);
  EXPECT_EQ(events[0].id, "like:item1");
  EXPECT_EQ(events[0].score, 100);
  EXPECT_EQ(events[0].type, EventType::SET);
}

TEST_F(EventTypeTest, SetTypeIdempotent) {
  EventStore store(config_);

  // SET to 100
  ASSERT_TRUE(store.AddEvent("user1", "like:item1", 100, EventType::SET));

  // SET to 100 again (idempotent)
  ASSERT_TRUE(store.AddEvent("user1", "like:item1", 100, EventType::SET));

  auto stats = store.GetStatistics();
  EXPECT_EQ(stats.total_events, 2);
  EXPECT_EQ(stats.deduped_events, 1);  // Second is duplicate
  EXPECT_EQ(stats.stored_events, 1);
}

TEST_F(EventTypeTest, SetTypeStateTransition) {
  EventStore store(config_);

  // SET to 100 (like ON)
  ASSERT_TRUE(store.AddEvent("user1", "like:item1", 100, EventType::SET));

  // SET to 0 (like OFF)
  ASSERT_TRUE(store.AddEvent("user1", "like:item1", 0, EventType::SET));

  auto stats = store.GetStatistics();
  EXPECT_EQ(stats.total_events, 2);
  EXPECT_EQ(stats.deduped_events, 0);  // State changed, not duplicate
  EXPECT_EQ(stats.stored_events, 2);

  auto events = store.GetEvents("user1");
  ASSERT_EQ(events.size(), 2);
  EXPECT_EQ(events[0].score, 100);  // First: ON
  EXPECT_EQ(events[1].score, 0);    // Second: OFF
}

TEST_F(EventTypeTest, SetTypeWeightedBookmark) {
  EventStore store(config_);

  // Bookmark with high priority
  ASSERT_TRUE(store.AddEvent("user1", "bookmark:item1", 100, EventType::SET));

  // Change to medium priority
  ASSERT_TRUE(store.AddEvent("user1", "bookmark:item1", 50, EventType::SET));

  // Try to set medium again (duplicate)
  ASSERT_TRUE(store.AddEvent("user1", "bookmark:item1", 50, EventType::SET));

  // Change to low priority
  ASSERT_TRUE(store.AddEvent("user1", "bookmark:item1", 20, EventType::SET));

  auto stats = store.GetStatistics();
  EXPECT_EQ(stats.total_events, 4);
  EXPECT_EQ(stats.deduped_events, 1);  // Third event is duplicate
  EXPECT_EQ(stats.stored_events, 3);   // 100 -> 50 -> 20

  auto events = store.GetEvents("user1");
  ASSERT_EQ(events.size(), 3);
  EXPECT_EQ(events[0].score, 100);
  EXPECT_EQ(events[1].score, 50);
  EXPECT_EQ(events[2].score, 20);
}

TEST_F(EventTypeTest, SetTypeMultiLevelRating) {
  EventStore store(config_);

  // ★3 (60 points)
  ASSERT_TRUE(store.AddEvent("user1", "rating:item1", 60, EventType::SET));

  // ★4 (80 points)
  ASSERT_TRUE(store.AddEvent("user1", "rating:item1", 80, EventType::SET));

  // ★5 (100 points)
  ASSERT_TRUE(store.AddEvent("user1", "rating:item1", 100, EventType::SET));

  // Try ★5 again (duplicate)
  ASSERT_TRUE(store.AddEvent("user1", "rating:item1", 100, EventType::SET));

  auto stats = store.GetStatistics();
  EXPECT_EQ(stats.total_events, 4);
  EXPECT_EQ(stats.deduped_events, 1);
  EXPECT_EQ(stats.stored_events, 3);
}

// ============================================================================
// DEL Type Tests
// ============================================================================

TEST_F(EventTypeTest, DelTypeBasic) {
  EventStore store(config_);

  auto result = store.AddEvent("user1", "like:item1", 0, EventType::DEL);
  ASSERT_TRUE(result);

  auto events = store.GetEvents("user1");
  ASSERT_EQ(events.size(), 1);
  EXPECT_EQ(events[0].id, "like:item1");
  EXPECT_EQ(events[0].score, 0);  // DEL always stores score=0
  EXPECT_EQ(events[0].type, EventType::DEL);
}

TEST_F(EventTypeTest, DelTypeIdempotent) {
  EventStore store(config_);

  // First DEL
  ASSERT_TRUE(store.AddEvent("user1", "like:item1", 0, EventType::DEL));

  // Second DEL (idempotent, already deleted)
  ASSERT_TRUE(store.AddEvent("user1", "like:item1", 0, EventType::DEL));

  auto stats = store.GetStatistics();
  EXPECT_EQ(stats.total_events, 2);
  EXPECT_EQ(stats.deduped_events, 1);  // Second is duplicate
  EXPECT_EQ(stats.stored_events, 1);
}

// ============================================================================
// Mixed Type Tests
// ============================================================================

TEST_F(EventTypeTest, MixedTypesInSameContext) {
  EventStore store(config_);

  // ADD type (stream events)
  ASSERT_TRUE(store.AddEvent("user1", "view:item1", 100, EventType::ADD));
  ASSERT_TRUE(store.AddEvent("user1", "click:item2", 95, EventType::ADD));

  // SET type (state events)
  ASSERT_TRUE(store.AddEvent("user1", "like:item1", 100, EventType::SET));
  ASSERT_TRUE(store.AddEvent("user1", "bookmark:item2", 80, EventType::SET));

  // DEL type
  ASSERT_TRUE(store.AddEvent("user1", "like:item3", 0, EventType::DEL));

  auto events = store.GetEvents("user1");
  EXPECT_EQ(events.size(), 5);

  auto stats = store.GetStatistics();
  EXPECT_EQ(stats.total_events, 5);
  EXPECT_EQ(stats.deduped_events, 0);
  EXPECT_EQ(stats.stored_events, 5);
}

TEST_F(EventTypeTest, RealWorldScenario) {
  EventStore store(config_);

  // User views item1 (stream event)
  ASSERT_TRUE(store.AddEvent("user1", "view:item1", 100, EventType::ADD));

  // User likes item1 (state event)
  ASSERT_TRUE(store.AddEvent("user1", "like:item1", 100, EventType::SET));

  // Network retry - likes item1 again (idempotent)
  ASSERT_TRUE(store.AddEvent("user1", "like:item1", 100, EventType::SET));

  // User bookmarks item1 with high priority
  ASSERT_TRUE(store.AddEvent("user1", "bookmark:item1", 100, EventType::SET));

  // User changes bookmark priority to medium
  ASSERT_TRUE(store.AddEvent("user1", "bookmark:item1", 50, EventType::SET));

  // User unlikes item1
  ASSERT_TRUE(store.AddEvent("user1", "like:item1", 0, EventType::SET));

  // User removes bookmark
  ASSERT_TRUE(store.AddEvent("user1", "bookmark:item1", 0, EventType::DEL));

  // Retry bookmark removal (idempotent)
  ASSERT_TRUE(store.AddEvent("user1", "bookmark:item1", 0, EventType::DEL));

  auto stats = store.GetStatistics();
  EXPECT_EQ(stats.total_events, 8);
  EXPECT_EQ(stats.deduped_events, 2);  // 1 like retry + 1 del retry
  EXPECT_EQ(stats.stored_events, 6);   // view, like, bookmark(100), bookmark(50), like(0), del

  auto events = store.GetEvents("user1");
  EXPECT_EQ(events.size(), 6);
}

TEST_F(EventTypeTest, SetAndDelSameId) {
  EventStore store(config_);

  // SET like to ON
  ASSERT_TRUE(store.AddEvent("user1", "like:item1", 100, EventType::SET));

  // SET like to OFF
  ASSERT_TRUE(store.AddEvent("user1", "like:item1", 0, EventType::SET));

  // DEL like (different from SET 0)
  ASSERT_TRUE(store.AddEvent("user1", "like:item1", 0, EventType::DEL));

  auto stats = store.GetStatistics();
  EXPECT_EQ(stats.total_events, 3);
  // Note: SET to 0, then DEL with 0 might be considered duplicate
  // This depends on state_cache implementation
}

}  // namespace nvecd::events
