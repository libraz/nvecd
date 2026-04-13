/**
 * @file co_occurrence_pruning_test.cpp
 * @brief Tests for CoOccurrenceIndex pruning and negative signal control
 */

#include "events/co_occurrence_index.h"

#include <gtest/gtest.h>

#include <cmath>
#include <string>
#include <vector>

namespace nvecd::events {
namespace {

std::vector<Event> MakeEvents(
    const std::vector<std::tuple<std::string, int, uint64_t>>& data) {
  std::vector<Event> events;
  for (const auto& [id, score, timestamp] : data) {
    events.emplace_back(id, score, timestamp);
  }
  return events;
}

// ============================================================================
// max_neighbors_per_item pruning
// ============================================================================

TEST(CoOccurrencePruningTest, MaxNeighborsLimitsOnUpdate) {
  CoOccurrenceIndex::Config cfg;
  cfg.max_neighbors_per_item = 2;
  CoOccurrenceIndex index(cfg);

  // item0 co-occurs with item1 (score 100), item2 (200), item3 (300), item4 (400)
  auto events = MakeEvents({
      {"item0", 10, 1000},
      {"item1", 10, 1001},
      {"item2", 20, 1002},
      {"item3", 30, 1003},
      {"item4", 40, 1004},
  });
  index.UpdateFromEvents("ctx1", events);

  // Each item should have at most 2 neighbors after inline pruning
  EXPECT_LE(index.GetNeighborCount("item0"), 2U);

  // The lowest-score neighbors for item0 (item1=100, item2=200) should be pruned
  // Note: cross-pruning by other items may also remove entries symmetrically
  EXPECT_FLOAT_EQ(index.GetScore("item0", "item1"), 0.0F);
}

TEST(CoOccurrencePruningTest, MaxNeighborsPreservesSymmetry) {
  CoOccurrenceIndex::Config cfg;
  cfg.max_neighbors_per_item = 2;
  CoOccurrenceIndex index(cfg);

  auto events = MakeEvents({
      {"item0", 10, 1000},
      {"item1", 10, 1001},
      {"item2", 20, 1002},
      {"item3", 30, 1003},
  });
  index.UpdateFromEvents("ctx1", events);

  // If item0->item1 was pruned, item1->item0 should also be pruned
  EXPECT_FLOAT_EQ(index.GetScore("item0", "item1"),
                  index.GetScore("item1", "item0"));
}

TEST(CoOccurrencePruningTest, MaxNeighborsZeroMeansUnlimited) {
  CoOccurrenceIndex::Config cfg;
  cfg.max_neighbors_per_item = 0;
  CoOccurrenceIndex index(cfg);

  auto events = MakeEvents({
      {"item0", 10, 1000},
      {"item1", 10, 1001},
      {"item2", 20, 1002},
      {"item3", 30, 1003},
      {"item4", 40, 1004},
  });
  index.UpdateFromEvents("ctx1", events);

  // All neighbors preserved
  EXPECT_EQ(index.GetNeighborCount("item0"), 4U);
}

TEST(CoOccurrencePruningTest, MaxNeighborsExactLimit) {
  CoOccurrenceIndex::Config cfg;
  cfg.max_neighbors_per_item = 3;
  CoOccurrenceIndex index(cfg);

  // Exactly 3 neighbors — no pruning needed
  auto events = MakeEvents({
      {"item0", 10, 1000},
      {"item1", 10, 1001},
      {"item2", 20, 1002},
      {"item3", 30, 1003},
  });
  index.UpdateFromEvents("ctx1", events);

  EXPECT_EQ(index.GetNeighborCount("item0"), 3U);
}

// ============================================================================
// min_support pruning
// ============================================================================

TEST(CoOccurrencePruningTest, MinSupportRemovesLowScores) {
  CoOccurrenceIndex::Config cfg;
  cfg.min_support = 50.0F;
  CoOccurrenceIndex index(cfg);

  // Scores: item0-item1=10 (pruned), item0-item2=100 (kept)
  auto events = MakeEvents({
      {"item0", 10, 1000},
      {"item1", 1, 1001},   // 10*1 = 10 < 50
      {"item2", 10, 1002},  // 10*10 = 100 >= 50
  });
  index.UpdateFromEvents("ctx1", events);

  // Manually prune (UpdateFromEvents only prunes max_neighbors, not min_support inline)
  index.Prune();

  EXPECT_FLOAT_EQ(index.GetScore("item0", "item1"), 0.0F);
  EXPECT_GT(index.GetScore("item0", "item2"), 0.0F);
}

TEST(CoOccurrencePruningTest, MinSupportSymmetricRemoval) {
  CoOccurrenceIndex::Config cfg;
  cfg.min_support = 200.0F;
  CoOccurrenceIndex index(cfg);

  index.SetScore("item1", "item2", 100.0F);  // Below min_support
  index.SetScore("item1", "item3", 300.0F);  // Above min_support

  index.Prune();

  EXPECT_FLOAT_EQ(index.GetScore("item1", "item2"), 0.0F);
  EXPECT_FLOAT_EQ(index.GetScore("item2", "item1"), 0.0F);
  EXPECT_FLOAT_EQ(index.GetScore("item1", "item3"), 300.0F);
}

TEST(CoOccurrencePruningTest, MinSupportZeroMeansNoPruning) {
  CoOccurrenceIndex::Config cfg;
  cfg.min_support = 0.0F;
  CoOccurrenceIndex index(cfg);

  index.SetScore("item1", "item2", 0.001F);
  index.Prune();

  EXPECT_GT(index.GetScore("item1", "item2"), 0.0F);
}

// ============================================================================
// Decay + min_support integration
// ============================================================================

TEST(CoOccurrencePruningTest, DecayUsesMinSupportAsThreshold) {
  CoOccurrenceIndex::Config cfg;
  cfg.min_support = 10.0F;
  CoOccurrenceIndex index(cfg);

  index.SetScore("item1", "item2", 20.0F);
  index.SetScore("item1", "item3", 100.0F);

  // After decay by 0.4: item1-item2 = 8.0 (< 10, pruned), item1-item3 = 40.0 (kept)
  index.ApplyDecay(0.4);

  EXPECT_FLOAT_EQ(index.GetScore("item1", "item2"), 0.0F);
  EXPECT_NEAR(index.GetScore("item1", "item3"), 40.0F, 0.1F);
}

TEST(CoOccurrencePruningTest, DecayWithoutMinSupportUsesDefault) {
  CoOccurrenceIndex index;  // Default config, min_support = 0

  index.SetScore("item1", "item2", 1.0F);

  // Default threshold is 1e-6F, so after moderate decay the entry survives
  index.ApplyDecay(0.5);

  EXPECT_GT(index.GetScore("item1", "item2"), 0.0F);
}

// ============================================================================
// Combined pruning (max_neighbors + min_support)
// ============================================================================

TEST(CoOccurrencePruningTest, CombinedPruning) {
  CoOccurrenceIndex::Config cfg;
  cfg.max_neighbors_per_item = 2;
  cfg.min_support = 50.0F;
  CoOccurrenceIndex index(cfg);

  // item0 neighbors: item1(10), item2(100), item3(200), item4(30)
  index.SetScore("item0", "item1", 10.0F);
  index.SetScore("item0", "item2", 100.0F);
  index.SetScore("item0", "item3", 200.0F);
  index.SetScore("item0", "item4", 30.0F);

  index.Prune();

  // min_support removes item1(10) and item4(30)
  // max_neighbors(2) keeps top-2: item3(200), item2(100)
  EXPECT_FLOAT_EQ(index.GetScore("item0", "item1"), 0.0F);
  EXPECT_FLOAT_EQ(index.GetScore("item0", "item4"), 0.0F);
  EXPECT_GT(index.GetScore("item0", "item3"), 0.0F);
  EXPECT_GT(index.GetScore("item0", "item2"), 0.0F);
  EXPECT_LE(index.GetNeighborCount("item0"), 2U);
}

// ============================================================================
// Explicit Prune() call
// ============================================================================

TEST(CoOccurrencePruningTest, PruneOnExistingData) {
  CoOccurrenceIndex::Config cfg;
  cfg.max_neighbors_per_item = 1;
  CoOccurrenceIndex index(cfg);

  // SetScore bypasses the inline pruning in UpdateFromEvents
  index.SetScore("item0", "item1", 100.0F);
  index.SetScore("item0", "item2", 200.0F);
  index.SetScore("item0", "item3", 300.0F);

  EXPECT_EQ(index.GetNeighborCount("item0"), 3U);

  index.Prune();

  // Only top-1 kept: item3(300)
  EXPECT_EQ(index.GetNeighborCount("item0"), 1U);
  EXPECT_GT(index.GetScore("item0", "item3"), 0.0F);
}

TEST(CoOccurrencePruningTest, PruneEmptyIndex) {
  CoOccurrenceIndex::Config cfg;
  cfg.max_neighbors_per_item = 5;
  cfg.min_support = 10.0F;
  CoOccurrenceIndex index(cfg);

  index.Prune();  // Should not crash
  EXPECT_EQ(index.GetItemCount(), 0U);
}

TEST(CoOccurrencePruningTest, PruneIncrementsGeneration) {
  CoOccurrenceIndex::Config cfg;
  cfg.max_neighbors_per_item = 1;
  CoOccurrenceIndex index(cfg);

  index.SetScore("item0", "item1", 100.0F);
  uint64_t gen_before = index.GetGeneration();

  index.Prune();

  EXPECT_GT(index.GetGeneration(), gen_before);
}

// ============================================================================
// Negative signal propagation control
// ============================================================================

TEST(CoOccurrencePruningTest, NegativePropagationDefault) {
  // Default propagation = 1 (direct neighbors affected)
  CoOccurrenceIndex index;

  auto events = MakeEvents({
      {"item1", 10, 1000},
      {"item2", 20, 1001},
  });
  index.UpdateFromEvents("ctx1", events);

  float before = index.GetScore("item1", "item2");
  EXPECT_GT(before, 0.0F);

  auto lock = index.AcquireWriteLock();
  index.ApplyNegativeSignalLocked("item1", events, 0.5);
  lock.unlock();

  float after = index.GetScore("item1", "item2");
  EXPECT_LT(after, before);
}

TEST(CoOccurrencePruningTest, NegativePropagationDisabled) {
  CoOccurrenceIndex::Config cfg;
  cfg.negative_max_propagation = 0;
  CoOccurrenceIndex index(cfg);

  auto events = MakeEvents({
      {"item1", 10, 1000},
      {"item2", 20, 1001},
  });
  index.UpdateFromEvents("ctx1", events);

  float before = index.GetScore("item1", "item2");

  auto lock = index.AcquireWriteLock();
  index.ApplyNegativeSignalLocked("item1", events, 0.5);
  lock.unlock();

  // Score should be unchanged (propagation disabled)
  EXPECT_FLOAT_EQ(index.GetScore("item1", "item2"), before);
}

// ============================================================================
// Default config backward compatibility
// ============================================================================

TEST(CoOccurrencePruningTest, DefaultConfigNoPruning) {
  CoOccurrenceIndex index;  // Default: max=0, min_support=0, propagation=1

  auto events = MakeEvents({
      {"item0", 10, 1000},
      {"item1", 1, 1001},
      {"item2", 2, 1002},
      {"item3", 3, 1003},
      {"item4", 4, 1004},
      {"item5", 5, 1005},
  });
  index.UpdateFromEvents("ctx1", events);

  // All neighbors preserved (no pruning)
  EXPECT_EQ(index.GetNeighborCount("item0"), 5U);
}

}  // namespace
}  // namespace nvecd::events
