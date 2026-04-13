/**
 * @file co_occurrence_negative_test.cpp
 * @brief Unit tests for negative signals feature
 */

#include <gtest/gtest.h>

#include "events/co_occurrence_index.h"

namespace nvecd::events {

TEST(CoOccurrenceNegativeTest, NegativeDisabledByDefault) {
  CoOccurrenceIndex index;

  // Add events to build co-occurrence
  std::vector<Event> events = {Event("a", 10, 1000), Event("b", 10, 1000), Event("c", 10, 1000)};
  index.UpdateFromEvents("ctx", events);

  float score_before = index.GetScore("a", "b");
  EXPECT_GT(score_before, 0.0F);

  // Without calling ApplyNegativeSignalLocked, DEL events don't affect scores
  // (this is the responsibility of the dispatcher, not the index itself)
}

TEST(CoOccurrenceNegativeTest, ReducesCooccurrence) {
  CoOccurrenceIndex index;

  std::vector<Event> events = {Event("a", 10, 1000), Event("b", 10, 1000), Event("c", 10, 1000)};
  index.UpdateFromEvents("ctx", events);

  float score_ab_before = index.GetScore("a", "b");
  float score_ac_before = index.GetScore("a", "c");

  // Apply negative signal for item "a"
  {
    auto lock = index.AcquireWriteLock();
    index.ApplyNegativeSignalLocked("a", events, 0.5);
  }

  float score_ab_after = index.GetScore("a", "b");
  float score_ac_after = index.GetScore("a", "c");

  EXPECT_LT(score_ab_after, score_ab_before);
  EXPECT_LT(score_ac_after, score_ac_before);
}

TEST(CoOccurrenceNegativeTest, SuppressedItemsNotReturned) {
  CoOccurrenceIndex index;

  std::vector<Event> events = {Event("a", 5, 1000), Event("b", 5, 1000)};
  index.UpdateFromEvents("ctx", events);

  // Score should be 5*5 = 25
  EXPECT_GT(index.GetScore("a", "b"), 0.0F);

  // Apply strong negative signal (weight > score product ratio)
  {
    auto lock = index.AcquireWriteLock();
    index.ApplyNegativeSignalLocked("a", events,
                                    10.0);  // Large weight to force negative
  }

  // Score should now be negative, which GetSimilar filters out
  auto similar = index.GetSimilar("a", 10);

  // b should not appear in results (score <= 0)
  for (const auto& [id, score] : similar) {
    EXPECT_NE(id, "b");
  }
}

TEST(CoOccurrenceNegativeTest, SymmetricReduction) {
  CoOccurrenceIndex index;

  std::vector<Event> events = {Event("a", 10, 1000), Event("b", 10, 1000)};
  index.UpdateFromEvents("ctx", events);

  {
    auto lock = index.AcquireWriteLock();
    index.ApplyNegativeSignalLocked("a", events, 0.5);
  }

  // Both directions should be equally reduced
  EXPECT_FLOAT_EQ(index.GetScore("a", "b"), index.GetScore("b", "a"));
}

TEST(CoOccurrenceNegativeTest, OnlyAffectsDeletedItem) {
  CoOccurrenceIndex index;

  std::vector<Event> events = {Event("a", 10, 1000), Event("b", 10, 1000), Event("c", 10, 1000)};
  index.UpdateFromEvents("ctx", events);

  float score_bc_before = index.GetScore("b", "c");

  // Apply negative signal for item "a" only
  {
    auto lock = index.AcquireWriteLock();
    index.ApplyNegativeSignalLocked("a", events, 0.5);
  }

  float score_bc_after = index.GetScore("b", "c");

  // b-c score should be unchanged
  EXPECT_FLOAT_EQ(score_bc_before, score_bc_after);
}

}  // namespace nvecd::events
