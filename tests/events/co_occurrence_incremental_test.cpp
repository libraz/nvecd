/**
 * @file co_occurrence_incremental_test.cpp
 * @brief Tests for incremental, atomic co-occurrence ingestion
 *
 * Covers the audit fixes for:
 * - C-3: incremental updates must add each pair exactly once (no O(N^2)
 *   over-count from re-scanning the whole buffer per event).
 * - H-2: EventStore::AddEventAndGetPrior + CoOccurrenceIndex::ApplyIngestedEvent
 *   must yield correct, deterministic pair counts under concurrent same-context
 *   ingestion.
 * - H-18: CoOccurrenceIndex::GetStatistics must not recursively re-lock.
 */

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "config/config.h"
#include "events/co_occurrence_index.h"
#include "events/event_store.h"

namespace nvecd::events {
namespace {

// Mirror of the shared ingestion routine in the TCP/HTTP EVENT handlers:
// atomically append the event, then apply the incremental co-occurrence delta.
void IngestEvent(EventStore& store, CoOccurrenceIndex& index, const std::string& ctx, const std::string& id, int score,
                 const CoOccurrenceIndex::IngestOptions& options, EventType type = EventType::ADD,
                 uint64_t timestamp = 1) {
  auto result = store.AddEventAndGetPrior(ctx, id, score, type, timestamp);
  ASSERT_TRUE(result.has_value());
  if (!result->deduped) {
    index.ApplyIngestedEvent(ctx, result->prior_events, result->stored_event, options);
  }
}

config::EventsConfig MakeConfig(uint32_t ctx_buffer_size) {
  config::EventsConfig cfg;
  cfg.ctx_buffer_size = ctx_buffer_size;
  // Disable time-window dedup so every distinct event is stored; we want to
  // exercise pair counting, not deduplication, in these tests.
  cfg.dedup_window_sec = 0;
  cfg.dedup_cache_size = 0;
  return cfg;
}

// ============================================================================
// Exact incremental counts (C-3)
// ============================================================================

// Feeding a fixed sequence of distinct items one-by-one must produce the same
// scores as a single from-scratch batch update -- NOT the inflated values that
// re-scanning the whole buffer per event would produce.
TEST(CoOccurrenceIncrementalTest, MatchesBatchForDistinctItems) {
  CoOccurrenceIndex index;
  index.AddEventIncremental("ctx", {}, Event("a", 2, 1), false, 0.0);  // no prior -> no-op
  index.AddEventIncremental("ctx", {Event("a", 2, 1)}, Event("b", 3, 1), false, 0.0);
  index.AddEventIncremental("ctx", {Event("a", 2, 1), Event("b", 3, 1)}, Event("c", 4, 1), false, 0.0);

  // Each unordered pair counted exactly once.
  EXPECT_FLOAT_EQ(index.GetScore("a", "b"), 6.0F);   // 2*3
  EXPECT_FLOAT_EQ(index.GetScore("a", "c"), 8.0F);   // 2*4
  EXPECT_FLOAT_EQ(index.GetScore("b", "c"), 12.0F);  // 3*4

  // Symmetric.
  EXPECT_FLOAT_EQ(index.GetScore("b", "a"), 6.0F);
  EXPECT_EQ(index.GetItemCount(), 3);

  // Cross-check against the batch full-scan path.
  CoOccurrenceIndex batch;
  batch.UpdateFromEvents("ctx", {Event("a", 2, 1), Event("b", 3, 1), Event("c", 4, 1)});
  EXPECT_FLOAT_EQ(index.GetScore("a", "b"), batch.GetScore("a", "b"));
  EXPECT_FLOAT_EQ(index.GetScore("a", "c"), batch.GetScore("a", "c"));
  EXPECT_FLOAT_EQ(index.GetScore("b", "c"), batch.GetScore("b", "c"));
}

// The old O(N^2) bug re-added (a,b) on every subsequent event. With N events,
// pair (a,b) would have been inflated ~N-1 times. Verify the incremental path
// adds it exactly once regardless of how many later events arrive.
TEST(CoOccurrenceIncrementalTest, PairNotReAddedOnLaterEvents) {
  CoOccurrenceIndex index;
  std::vector<Event> prior;

  // a, b first (forms the (a,b) pair once).
  index.AddEventIncremental("ctx", prior, Event("a", 1, 1), false, 0.0);
  prior.emplace_back("a", 1, 1);
  index.AddEventIncremental("ctx", prior, Event("b", 1, 1), false, 0.0);
  prior.emplace_back("b", 1, 1);

  // Now 8 more distinct items arrive. None should touch the (a,b) score.
  for (int i = 0; i < 8; ++i) {
    std::string id = "x" + std::to_string(i);
    index.AddEventIncremental("ctx", prior, Event(id, 1, 1), false, 0.0);
    prior.emplace_back(id, 1, 1);
  }

  EXPECT_FLOAT_EQ(index.GetScore("a", "b"), 1.0F);  // 1*1, added exactly once
}

// Repeated occurrences of the same item accumulate, matching the batch result.
TEST(CoOccurrenceIncrementalTest, AccumulatesRepeatedPairs) {
  CoOccurrenceIndex index;
  std::vector<Event> prior;

  // a, b, a -> pair (a,b) appears twice (b vs first a, then second a vs b).
  index.AddEventIncremental("ctx", prior, Event("a", 2, 1), false, 0.0);
  prior.emplace_back("a", 2, 1);
  index.AddEventIncremental("ctx", prior, Event("b", 3, 1), false, 0.0);  // (a,b)=6
  prior.emplace_back("b", 3, 1);
  index.AddEventIncremental("ctx", prior, Event("a", 2, 1), false, 0.0);  // +(a,b)=6 (second a vs b)

  CoOccurrenceIndex batch;
  batch.UpdateFromEvents("ctx", {Event("a", 2, 1), Event("b", 3, 1), Event("a", 2, 1)});
  EXPECT_FLOAT_EQ(index.GetScore("a", "b"), batch.GetScore("a", "b"));
  EXPECT_FLOAT_EQ(index.GetScore("a", "b"), 12.0F);
}

// Self-pairs (same item id as a prior event) must be skipped.
TEST(CoOccurrenceIncrementalTest, SkipsSelfPairs) {
  CoOccurrenceIndex index;
  index.AddEventIncremental("ctx", {Event("a", 5, 1)}, Event("a", 7, 1), false, 0.0);
  EXPECT_EQ(index.GetScore("a", "a"), 0.0F);
  EXPECT_EQ(index.GetItemCount(), 0);
}

// ============================================================================
// Ring-buffer eviction (C-3 with > ctx_buffer_size events)
// ============================================================================

// When more events arrive than the context buffer holds, the prior view used
// for incremental scoring is only the retained window. Driving ingestion
// through EventStore must match a batch computation over the surviving window.
TEST(CoOccurrenceIncrementalTest, RingBufferEvictionMatchesWindowedBatch) {
  constexpr uint32_t kBufferSize = 3;
  EventStore store(MakeConfig(kBufferSize));
  CoOccurrenceIndex index;
  CoOccurrenceIndex::IngestOptions options;

  // Feed 5 distinct items into a buffer of size 3.
  const std::vector<std::string> ids = {"a", "b", "c", "d", "e"};
  for (size_t i = 0; i < ids.size(); ++i) {
    IngestEvent(store, index, "ctx", ids[i], static_cast<int>(i + 1), options);
  }

  // Scores (a=1,b=2,c=3,d=4,e=5). The prior buffer is captured *before* each
  // push, and eviction happens on push, so when an event arrives it pairs
  // against whatever the buffer held just before. Trace:
  //   push b: prior {a}        -> (a,b)=2
  //   push c: prior {a,b}      -> (a,c)=3, (b,c)=6
  //   push d: prior {a,b,c}    -> (a,d)=4, (b,d)=8, (c,d)=12 ; then evict a
  //   push e: prior {b,c,d}    -> (b,e)=10,(c,e)=15,(d,e)=20 ; then evict b
  EXPECT_FLOAT_EQ(index.GetScore("a", "b"), 2.0F);
  EXPECT_FLOAT_EQ(index.GetScore("a", "d"), 4.0F);  // a still present when d arrived
  EXPECT_FLOAT_EQ(index.GetScore("c", "d"), 12.0F);
  EXPECT_FLOAT_EQ(index.GetScore("d", "e"), 20.0F);

  // 'a' was evicted before 'e' arrived, so (a,e) must never have formed.
  EXPECT_FLOAT_EQ(index.GetScore("a", "e"), 0.0F);
  // Every pair appears at most once: total unordered pairs formed = 9.
  EXPECT_EQ(index.GetStatistics().co_pairs, 9U);
}

// ============================================================================
// Atomic ingestion under concurrency (H-2)
// ============================================================================

// Many threads firing EVENTs on the SAME context concurrently. Because each
// append atomically captures a distinct prior view, each unordered pair is
// counted exactly once regardless of interleaving -- the final scores must be
// deterministic and match the single-threaded result.
TEST(CoOccurrenceIncrementalTest, ConcurrentSameContextIsDeterministic) {
  constexpr int kNumItems = 64;
  constexpr uint32_t kBufferSize = kNumItems;  // large enough to retain all

  // Compute the expected result single-threaded: all pairs (i,j) with score 1.
  // Each unordered pair {i,j} contributes exactly 1*1 = 1 once.
  auto run_once = [&](bool concurrent) {
    EventStore store(MakeConfig(kBufferSize));
    CoOccurrenceIndex index;
    CoOccurrenceIndex::IngestOptions options;

    if (concurrent) {
      std::vector<std::thread> threads;
      threads.reserve(kNumItems);
      for (int i = 0; i < kNumItems; ++i) {
        threads.emplace_back([&store, &index, &options, i]() {
          auto result = store.AddEventAndGetPrior("ctx", "item" + std::to_string(i), 1, EventType::ADD, 1);
          ASSERT_TRUE(result.has_value());
          if (!result->deduped) {
            index.ApplyIngestedEvent("ctx", result->prior_events, result->stored_event, options);
          }
        });
      }
      for (auto& t : threads) {
        t.join();
      }
    } else {
      for (int i = 0; i < kNumItems; ++i) {
        IngestEvent(store, index, "ctx", "item" + std::to_string(i), 1, options);
      }
    }
    return index.GetStatistics();
  };

  auto seq_stats = run_once(false);
  auto conc_stats = run_once(true);

  // N items fully connected -> N*(N-1)/2 unordered pairs.
  EXPECT_EQ(seq_stats.co_pairs, static_cast<size_t>(kNumItems) * (kNumItems - 1) / 2);

  // The concurrent run must produce exactly the same pair count: no
  // double-counts, no lost pairs.
  EXPECT_EQ(conc_stats.co_pairs, seq_stats.co_pairs);
  EXPECT_EQ(conc_stats.tracked_ids, seq_stats.tracked_ids);

  // Every pair score must be exactly 1.0 (added once), never 2.0 (double).
  // Re-run concurrently and inspect all scores.
  EventStore store(MakeConfig(kBufferSize));
  CoOccurrenceIndex index;
  CoOccurrenceIndex::IngestOptions options;
  {
    std::vector<std::thread> threads;
    threads.reserve(kNumItems);
    for (int i = 0; i < kNumItems; ++i) {
      threads.emplace_back([&store, &index, &options, i]() {
        auto result = store.AddEventAndGetPrior("ctx", "item" + std::to_string(i), 1, EventType::ADD, 1);
        ASSERT_TRUE(result.has_value());
        index.ApplyIngestedEvent("ctx", result->prior_events, result->stored_event, options);
      });
    }
    for (auto& t : threads) {
      t.join();
    }
  }
  for (int i = 0; i < kNumItems; ++i) {
    for (int j = i + 1; j < kNumItems; ++j) {
      float score = index.GetScore("item" + std::to_string(i), "item" + std::to_string(j));
      EXPECT_FLOAT_EQ(score, 1.0F) << "pair (" << i << "," << j << ") was not counted exactly once";
    }
  }
}

// ============================================================================
// H-18: GetStatistics must not recursively re-lock
// ============================================================================

// A direct call: before the fix, GetStatistics() called the public
// MemoryUsage(), re-acquiring the non-recursive shared_mutex (UB). This now
// returns cleanly.
TEST(CoOccurrenceIncrementalTest, GetStatisticsDoesNotReLock) {
  CoOccurrenceIndex index;
  index.UpdateFromEvents("ctx", {Event("a", 1, 1), Event("b", 2, 1), Event("c", 3, 1)});

  auto stats = index.GetStatistics();
  EXPECT_EQ(stats.tracked_ids, 3U);
  EXPECT_EQ(stats.co_pairs, 3U);
  EXPECT_GT(stats.memory_bytes, 0U);

  // MemoryUsage() (public, takes the lock once) must also agree.
  EXPECT_EQ(stats.memory_bytes, index.MemoryUsage());
}

// GetStatistics() called repeatedly while writers run concurrently must not
// deadlock or hang. Bounded by a timeout: if a recursive-lock regression
// returned, a writer queued between the two shared_locks would block readers.
TEST(CoOccurrenceIncrementalTest, GetStatisticsConcurrentWithWritersNoDeadlock) {
  CoOccurrenceIndex index;
  index.UpdateFromEvents("ctx", {Event("seed1", 1, 1), Event("seed2", 1, 1)});

  std::atomic<bool> stop{false};
  std::atomic<uint64_t> stats_calls{0};

  std::vector<std::thread> writers;
  for (int w = 0; w < 4; ++w) {
    writers.emplace_back([&index, &stop, w]() {
      int counter = 0;
      while (!stop.load()) {
        std::vector<Event> events = {Event("w" + std::to_string(w), 1, 1),
                                     Event("v" + std::to_string(counter++), 1, 1)};
        index.UpdateFromEvents("ctx", events);
      }
    });
  }

  std::vector<std::thread> readers;
  for (int r = 0; r < 4; ++r) {
    readers.emplace_back([&index, &stop, &stats_calls]() {
      while (!stop.load()) {
        auto stats = index.GetStatistics();
        (void)stats;
        index.MemoryUsage();
        stats_calls.fetch_add(1);
      }
    });
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  stop.store(true);
  for (auto& t : writers) {
    t.join();
  }
  for (auto& t : readers) {
    t.join();
  }

  // If we got here without hanging, no deadlock occurred and stats were served.
  EXPECT_GT(stats_calls.load(), 0U);
}

// ============================================================================
// Negative signals on DEL via the shared ingestion routine
// ============================================================================

// A DEL event with negative signals enabled dampens the removed item's
// associations with its prior co-occurring items.
TEST(CoOccurrenceIncrementalTest, NegativeSignalReducesScoreOnDelete) {
  EventStore store(MakeConfig(16));
  CoOccurrenceIndex index;

  CoOccurrenceIndex::IngestOptions options;
  options.negative_signals = true;
  options.negative_weight = 0.5;

  // Build co-occurrence between a and b.
  IngestEvent(store, index, "ctx", "a", 4, options);
  IngestEvent(store, index, "ctx", "b", 4, options);
  EXPECT_FLOAT_EQ(index.GetScore("a", "b"), 16.0F);  // 4*4

  // DEL 'a' -> reduce a<->b by b.score * negative_weight = 4 * 0.5 = 2.
  IngestEvent(store, index, "ctx", "a", 0, options, EventType::DEL);
  EXPECT_FLOAT_EQ(index.GetScore("a", "b"), 14.0F);
}

}  // namespace
}  // namespace nvecd::events
