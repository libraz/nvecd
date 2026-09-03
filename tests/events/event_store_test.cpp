/**
 * @file event_store_test.cpp
 * @brief Unit tests for EventStore
 */

#include "events/event_store.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <string>
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
  EXPECT_EQ(events[0].item_id, "item1");
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
  EXPECT_EQ(events[0].item_id, "item1");
  EXPECT_EQ(events[1].item_id, "item2");
  EXPECT_EQ(events[2].item_id, "item3");
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
  EXPECT_EQ(events1[0].item_id, "item1");

  auto events2 = store.GetEvents("user2");
  ASSERT_EQ(events2.size(), 1);
  EXPECT_EQ(events2[0].item_id, "item2");
}

TEST(EventStoreTest, MaxContextsEvictsLeastRecentlyWrittenContext) {
  auto config = MakeConfig();
  config.max_contexts = 2;
  EventStore store(config);

  ASSERT_TRUE(store.AddEvent("oldest", "item1", 10).has_value());
  ASSERT_TRUE(store.AddEvent("recent", "item2", 20).has_value());
  // Touch oldest so recent becomes the LRU entry.
  ASSERT_TRUE(store.AddEvent("oldest", "item3", 30).has_value());
  ASSERT_TRUE(store.AddEvent("new", "item4", 40).has_value());

  EXPECT_EQ(store.GetContextCount(), 2U);
  EXPECT_FALSE(store.GetEvents("oldest").empty());
  EXPECT_TRUE(store.GetEvents("recent").empty());
  EXPECT_FALSE(store.GetEvents("new").empty());
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
  EXPECT_EQ(events[0].item_id, "item2");
  EXPECT_EQ(events[1].item_id, "item3");
  EXPECT_EQ(events[2].item_id, "item4");
}

TEST(EventStoreTest, MultipleOverwrites) {
  auto config = MakeConfig(2);  // Very small buffer
  EventStore store(config);

  for (int i = 1; i <= 10; ++i) {
    ASSERT_TRUE(store.AddEvent("user1", "item" + std::to_string(i), i * 10).has_value());
  }

  EXPECT_EQ(store.GetTotalEventCount(), 10);

  auto events = store.GetEvents("user1");
  ASSERT_EQ(events.size(), 2);
  EXPECT_EQ(events[0].item_id, "item9");
  EXPECT_EQ(events[1].item_id, "item10");
}

// ============================================================================
// Validation
// ============================================================================

TEST(EventStoreTest, EmptyContext) {
  auto config = MakeConfig();
  EventStore store(config);

  auto result = store.AddEvent("", "item1", 10);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), utils::ErrorCode::kEventStoreError);
  EXPECT_NE(result.error().message().find("Context"), std::string::npos);

  EXPECT_EQ(store.GetContextCount(), 0);
  EXPECT_EQ(store.GetTotalEventCount(), 0);
}

TEST(EventStoreTest, EmptyId) {
  auto config = MakeConfig();
  EventStore store(config);

  auto result = store.AddEvent("user1", "", 10);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), utils::ErrorCode::kEventStoreError);
  EXPECT_NE(result.error().message().find("ID"), std::string::npos);

  EXPECT_EQ(store.GetContextCount(), 0);
  EXPECT_EQ(store.GetTotalEventCount(), 0);
}

TEST(EventStoreTest, NegativeScore) {
  auto config = MakeConfig();
  EventStore store(config);

  // Negative scores are out of the documented [0, 100] range and rejected.
  auto result = store.AddEvent("user1", "item1", -5);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), utils::ErrorCode::kEventInvalidScore);

  EXPECT_EQ(store.GetContextCount(), 0);
}

TEST(EventStoreTest, ScoreAboveMaxRejected) {
  auto config = MakeConfig();
  EventStore store(config);

  // Scores above the documented maximum of 100 are rejected.
  auto result = store.AddEvent("user1", "item1", 101);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), utils::ErrorCode::kEventInvalidScore);

  EXPECT_EQ(store.GetContextCount(), 0);
}

TEST(EventStoreTest, ZeroScore) {
  auto config = MakeConfig();
  EventStore store(config);

  // Zero scores are at the lower bound of the valid range and accepted.
  auto result = store.AddEvent("user1", "item1", 0);
  ASSERT_TRUE(result.has_value());

  auto events = store.GetEvents("user1");
  ASSERT_EQ(events.size(), 1);
  EXPECT_EQ(events[0].score, 0);
}

TEST(EventStoreTest, MaxScoreAccepted) {
  auto config = MakeConfig();
  EventStore store(config);

  // The documented maximum of 100 is accepted.
  auto result = store.AddEvent("user1", "item1", 100);
  ASSERT_TRUE(result.has_value());

  auto events = store.GetEvents("user1");
  ASSERT_EQ(events.size(), 1);
  EXPECT_EQ(events[0].score, 100);
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
  EXPECT_EQ(events[0].item_id, "item2");
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
        auto result = store.AddEvent(ctx, "item" + std::to_string(i), i);
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
    ASSERT_TRUE(store.AddEvent("user1", "item" + std::to_string(i), i).has_value());
  }

  std::atomic<bool> stop{false};
  std::atomic<int> read_count{0};

  // Writer thread
  std::thread writer([&store, &stop]() {
    int counter = 100;
    while (!stop.load()) {
      // Keep the score within the valid [0, 100] range while item IDs stay unique.
      store.AddEvent("user1", "item" + std::to_string(counter), counter % 101);
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
  EXPECT_EQ(events[0].item_id, long_id);
}

TEST(EventStoreTest, SpecialCharacters) {
  auto config = MakeConfig();
  EventStore store(config);

  std::string special_ctx = "user@#$%^&*()";
  std::string special_id = "item:42/\xE3\x81\x82";

  auto result = store.AddEvent(special_ctx, special_id, 10);
  ASSERT_TRUE(result.has_value()) << result.error().message();

  auto events = store.GetEvents(special_ctx);
  ASSERT_EQ(events.size(), 1);
  EXPECT_EQ(events[0].item_id, special_id);
}

TEST(EventStoreTest, FramingBreakingIdentifiersRejected) {
  auto config = MakeConfig();
  EventStore store(config);

  // Ids are written unescaped into the count-framed response, so an id able to
  // carry a newline or a space lets one surface corrupt another's framing.
  const std::vector<std::string> framing_breaking_ids = {"item\n", "item\r\nOK", "item id", "item\t2",
                                                         std::string("item\0x", 6)};
  for (const std::string& id : framing_breaking_ids) {
    auto result = store.AddEvent("user1", id, 10);
    EXPECT_FALSE(result.has_value()) << "accepted id: " << id;
  }
  for (const std::string& ctx : {"user\n1", "user 1"}) {
    auto result = store.AddEvent(ctx, "item1", 10);
    EXPECT_FALSE(result.has_value()) << "accepted ctx: " << ctx;
  }

  EXPECT_EQ(store.GetContextCount(), 0);
  EXPECT_EQ(store.GetTotalEventCount(), 0);
}

// ============================================================================
// Live / Restore Validation Symmetry
// ============================================================================

namespace {

// One candidate event in the exact form it would be stored. A row is either
// storable by both paths or rejected by both: the restore path may keep score
// values, types and timestamps verbatim, but it is not exempt from the
// invariants those values have to satisfy.
struct StoredFormCase {
  const char* name;
  std::string ctx;
  Event event;
  bool storable;
};

std::vector<StoredFormCase> StoredFormCases() {
  return {
      {"add lower bound", "user1", Event("item1", 0, 1000, EventType::ADD), true},
      {"add upper bound", "user1", Event("item2", 100, 1001, EventType::ADD), true},
      {"set mid range", "user1", Event("item3", 50, 1002, EventType::SET), true},
      {"del zero score", "user1", Event("item4", 0, 1003, EventType::DEL), true},
      {"add above range", "user1", Event("item5", 101, 1004, EventType::ADD), false},
      {"add negative score", "user1", Event("item6", -1, 1005, EventType::ADD), false},
      {"set far above range", "user1", Event("item7", 1000000, 1006, EventType::SET), false},
      {"del sentinel score", "user1", Event("item8", -1, 1007, EventType::DEL), false},
      {"del weighted score", "user1", Event("item9", 5, 1008, EventType::DEL), false},
      {"empty context", "", Event("item10", 10, 1009, EventType::ADD), false},
      {"empty id", "user1", Event("", 10, 1010, EventType::ADD), false},
      {"id with newline", "user1", Event("item\n11", 10, 1011, EventType::ADD), false},
      {"id with space", "user1", Event("item 12", 10, 1012, EventType::ADD), false},
      {"context with carriage return", "user\r1", Event("item13", 10, 1013, EventType::ADD), false},
  };
}

}  // namespace

TEST(EventStoreTest, RestorePathRejectsEverythingTheLivePathRejects) {
  for (const auto& test_case : StoredFormCases()) {
    auto config = MakeConfig();
    EventStore store(config);

    auto restored = store.RestoreEvent(test_case.ctx, test_case.event);
    EXPECT_EQ(restored.has_value(), test_case.storable) << test_case.name;
    if (!restored) {
      EXPECT_EQ(store.GetContextCount(), 0U) << test_case.name;
    } else {
      auto events = store.GetEvents(test_case.ctx);
      ASSERT_EQ(events.size(), 1U) << test_case.name;
      // Accepted rows come back byte-for-byte, timestamp included.
      EXPECT_EQ(events[0].item_id, test_case.event.item_id) << test_case.name;
      EXPECT_EQ(events[0].score, test_case.event.score) << test_case.name;
      EXPECT_EQ(events[0].timestamp, test_case.event.timestamp) << test_case.name;
      EXPECT_EQ(events[0].type, test_case.event.type) << test_case.name;
    }
  }
}

TEST(EventStoreTest, LivePathStoresExactlyTheFormsTheRestorePathAccepts) {
  for (const auto& test_case : StoredFormCases()) {
    auto config = MakeConfig();
    EventStore store(config);

    auto live = store.AddEvent(test_case.ctx, test_case.event.item_id, test_case.event.score, test_case.event.type,
                               test_case.event.timestamp);
    if (test_case.storable) {
      ASSERT_TRUE(live.has_value()) << test_case.name << ": " << live.error().message();
      auto events = store.GetEvents(test_case.ctx);
      ASSERT_EQ(events.size(), 1U) << test_case.name;
      EXPECT_EQ(events[0].score, test_case.event.score) << test_case.name;
      EXPECT_EQ(events[0].timestamp, test_case.event.timestamp) << test_case.name;
      continue;
    }

    // A non-storable row is either refused outright or normalized into a
    // different, storable form (a DEL always stores score 0). What must never
    // happen is the row reaching a buffer in the form the restore path refuses.
    if (live.has_value()) {
      EXPECT_EQ(test_case.event.type, EventType::DEL) << test_case.name;
      auto events = store.GetEvents(test_case.ctx);
      ASSERT_EQ(events.size(), 1U) << test_case.name;
      EXPECT_EQ(events[0].score, 0) << test_case.name;
    } else {
      EXPECT_EQ(store.GetContextCount(), 0U) << test_case.name;
    }
  }
}

TEST(EventStoreTest, PrepareEventAgreesWithTheStoredForm) {
  for (const auto& test_case : StoredFormCases()) {
    auto config = MakeConfig();
    EventStore store(config);

    auto prepared = store.PrepareEvent(test_case.ctx, test_case.event.item_id, test_case.event.score,
                                       test_case.event.type, test_case.event.timestamp);
    auto live = store.AddEvent(test_case.ctx, test_case.event.item_id, test_case.event.score, test_case.event.type,
                               test_case.event.timestamp);
    ASSERT_EQ(prepared.has_value(), live.has_value()) << test_case.name;
    if (prepared) {
      auto restore_check = store.RestoreEvent("preview", prepared->event);
      EXPECT_TRUE(restore_check.has_value()) << test_case.name << ": " << restore_check.error().message();
    }
  }
}

// ============================================================================
// LRU Bookkeeping (incremental vs full-scan reference)
// ============================================================================

namespace {

// Full-scan reference model of the context LRU: it recomputes the victim by
// scanning every tracked context, which is what the store used to do inline.
// It lives here so production code cannot reach it, and so a future edit that
// forgets to maintain the incremental structure fails mechanically.
class LruReference {
 public:
  void Write(const std::string& ctx, uint32_t max_contexts) {
    if (recency_.find(ctx) == recency_.end() && max_contexts > 0 && recency_.size() >= max_contexts) {
      const auto victim = std::min_element(recency_.begin(), recency_.end(),
                                           [](const auto& lhs, const auto& rhs) { return lhs.second < rhs.second; });
      recency_.erase(victim);
    }
    recency_[ctx] = ++sequence_;
  }

  void Clear() { recency_.clear(); }

  void Swap(LruReference& other) {
    recency_.swap(other.recency_);
    std::swap(sequence_, other.sequence_);
  }

  std::vector<std::string> Contexts() const {
    std::vector<std::string> contexts;
    contexts.reserve(recency_.size());
    for (const auto& [ctx, _] : recency_) {
      contexts.push_back(ctx);
    }
    std::sort(contexts.begin(), contexts.end());
    return contexts;
  }

 private:
  std::map<std::string, uint64_t> recency_;
  uint64_t sequence_ = 0;
};

std::vector<std::string> SortedContexts(const EventStore& store) {
  auto contexts = store.GetAllContexts();
  std::sort(contexts.begin(), contexts.end());
  return contexts;
}

}  // namespace

TEST(EventStoreTest, IncrementalLruMatchesFullScanReference) {
  auto config = MakeConfig(4);
  config.max_contexts = 8;
  EventStore store(config);
  LruReference reference;

  // Deterministic mixed workload: new contexts, repeat writes to existing ones,
  // and restores, so every mutation kind that touches recency is exercised.
  uint32_t state = 12345;
  const auto next = [&state]() {
    state = (state * 1664525U) + 1013904223U;
    return state;
  };

  for (int i = 0; i < 400; ++i) {
    const std::string ctx = "ctx" + std::to_string(next() % 20);
    const std::string item = "item" + std::to_string(i);
    if (i % 5 == 0) {
      ASSERT_TRUE(store.RestoreEvent(ctx, Event(item, 7, 2000 + static_cast<uint64_t>(i), EventType::SET)).has_value());
    } else {
      ASSERT_TRUE(store.AddEvent(ctx, item, 7).has_value());
    }
    reference.Write(ctx, config.max_contexts);

    ASSERT_EQ(SortedContexts(store), reference.Contexts()) << "diverged at write " << i;
    ASSERT_LE(store.GetContextCount(), config.max_contexts) << "diverged at write " << i;
  }
}

TEST(EventStoreTest, ClearResetsLruBookkeeping) {
  auto config = MakeConfig(4);
  config.max_contexts = 3;
  EventStore store(config);
  LruReference reference;

  for (int i = 0; i < 6; ++i) {
    const std::string ctx = "ctx" + std::to_string(i);
    ASSERT_TRUE(store.AddEvent(ctx, "item" + std::to_string(i), 7).has_value());
    reference.Write(ctx, config.max_contexts);
  }
  ASSERT_EQ(SortedContexts(store), reference.Contexts());

  store.Clear();
  reference.Clear();
  EXPECT_EQ(SortedContexts(store), reference.Contexts());

  // A stale entry left behind by Clear() would evict the wrong context here.
  for (int i = 0; i < 5; ++i) {
    const std::string ctx = "fresh" + std::to_string(i);
    ASSERT_TRUE(store.AddEvent(ctx, "item" + std::to_string(i), 7).has_value());
    reference.Write(ctx, config.max_contexts);
    ASSERT_EQ(SortedContexts(store), reference.Contexts()) << "diverged at write " << i;
  }
}

TEST(EventStoreTest, SwapStateMovesLruBookkeepingWithTheContexts) {
  auto config = MakeConfig(4);
  config.max_contexts = 3;
  EventStore store(config);
  EventStore staged(config);
  LruReference store_reference;
  LruReference staged_reference;

  for (int i = 0; i < 3; ++i) {
    const std::string ctx = "live" + std::to_string(i);
    ASSERT_TRUE(store.AddEvent(ctx, "item" + std::to_string(i), 7).has_value());
    store_reference.Write(ctx, config.max_contexts);
  }
  for (int i = 0; i < 3; ++i) {
    const std::string ctx = "staged" + std::to_string(i);
    ASSERT_TRUE(staged.RestoreEvent(ctx, Event("item" + std::to_string(i), 7, 3000, EventType::ADD)).has_value());
    staged_reference.Write(ctx, config.max_contexts);
  }

  store.SwapState(staged);
  store_reference.Swap(staged_reference);
  ASSERT_EQ(SortedContexts(store), store_reference.Contexts());
  ASSERT_EQ(SortedContexts(staged), staged_reference.Contexts());

  // Eviction after the swap must follow the recency that came with the state.
  for (int i = 0; i < 4; ++i) {
    const std::string ctx = "after" + std::to_string(i);
    ASSERT_TRUE(store.AddEvent(ctx, "item" + std::to_string(i), 7).has_value());
    store_reference.Write(ctx, config.max_contexts);
    ASSERT_EQ(SortedContexts(store), store_reference.Contexts()) << "diverged at write " << i;
  }
}

// ============================================================================
// Memory Accounting (in-place scan vs full-copy reference)
// ============================================================================

namespace {

// Full-copy reference estimate: it materializes every stored event the way the
// accessor used to, and must agree with the in-place traversal exactly.
// Identifiers are kept short so the copies carry the same string capacity as
// the stored originals.
size_t ReferenceMemoryUsage(const EventStore& store, uint32_t ctx_buffer_size) {
  size_t total = sizeof(EventStore);
  for (const auto& ctx : store.GetAllContexts()) {
    total += sizeof(std::string) + ctx.capacity();
    for (const auto& event : store.GetEvents(ctx)) {
      total += sizeof(Event);
      total += event.item_id.capacity();
    }
    total += sizeof(RingBuffer<Event>);
    total += ctx_buffer_size * sizeof(Event);
  }
  return total;
}

}  // namespace

TEST(EventStoreTest, MemoryUsageMatchesFullCopyReference) {
  constexpr uint32_t kBufferSize = 3;
  auto config = MakeConfig(kBufferSize);
  config.max_contexts = 4;
  EventStore store(config);

  EXPECT_EQ(store.MemoryUsage(), ReferenceMemoryUsage(store, kBufferSize)) << "empty store";

  // Append, ring-buffer overwrite, restore, context eviction and clear: every
  // mutation kind that can change the accounted set.
  for (int i = 0; i < 12; ++i) {
    ASSERT_TRUE(store.AddEvent("ctx" + std::to_string(i % 6), "item" + std::to_string(i), 7).has_value());
    EXPECT_EQ(store.MemoryUsage(), ReferenceMemoryUsage(store, kBufferSize)) << "after add " << i;
    EXPECT_EQ(store.GetStatistics().memory_bytes, ReferenceMemoryUsage(store, kBufferSize)) << "after add " << i;
  }

  for (int i = 0; i < 4; ++i) {
    ASSERT_TRUE(store.RestoreEvent("ctx" + std::to_string(i), Event("re" + std::to_string(i), 7, 4000, EventType::SET))
                    .has_value());
    EXPECT_EQ(store.MemoryUsage(), ReferenceMemoryUsage(store, kBufferSize)) << "after restore " << i;
  }

  store.Clear();
  EXPECT_EQ(store.MemoryUsage(), ReferenceMemoryUsage(store, kBufferSize)) << "after clear";
}

}  // namespace
}  // namespace nvecd::events
