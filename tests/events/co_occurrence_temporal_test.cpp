/**
 * @file co_occurrence_temporal_test.cpp
 * @brief Unit tests for temporal co-occurrence feature
 */

#include <gtest/gtest.h>

#include <cmath>

#include "events/co_occurrence_index.h"

namespace nvecd::events {

// Helper to create events with specific timestamps
Event MakeEvent(const std::string& id, int score, uint64_t timestamp) {
  return Event(id, score, timestamp, EventType::ADD);
}

TEST(CoOccurrenceTemporalTest, TemporalDisabledByDefault) {
  CoOccurrenceIndex index;

  // Without temporal (existing behavior)
  std::vector<Event> events1 = {MakeEvent("a", 10, 1000),
                                MakeEvent("b", 10, 1000)};
  index.UpdateFromEvents("ctx", events1, false, 0.0);

  float score_no_temporal = index.GetScore("a", "b");

  // Reset
  index.Clear();

  // Same events, temporal disabled should produce identical scores
  index.UpdateFromEvents("ctx", events1, false, 86400.0);
  float score_disabled = index.GetScore("a", "b");

  EXPECT_FLOAT_EQ(score_no_temporal, score_disabled);
}

TEST(CoOccurrenceTemporalTest, RecentEventsScoreHigher) {
  CoOccurrenceIndex index;

  // Old events
  std::vector<Event> events = {MakeEvent("a", 10, 1000), MakeEvent("b", 10, 1000),
                               MakeEvent("c", 10, 9000), MakeEvent("d", 10, 9000)};

  index.UpdateFromEvents("ctx", events, true, 3600.0);  // 1 hour half-life

  // c-d co-occurrence (recent, timestamps close to max) should be higher
  // than a-b co-occurrence (old, timestamps far from max)
  float score_cd = index.GetScore("c", "d");
  float score_ab = index.GetScore("a", "b");

  EXPECT_GT(score_cd, score_ab);
  EXPECT_GT(score_cd, 0.0F);
  EXPECT_GT(score_ab, 0.0F);
}

TEST(CoOccurrenceTemporalTest, HalfLifeCorrectness) {
  // Test that score at exactly one half-life is approximately half
  CoOccurrenceIndex index1;
  CoOccurrenceIndex index2;

  double half_life = 100.0;  // 100 seconds

  // Both events at same time (no decay)
  std::vector<Event> events_same = {MakeEvent("a", 10, 1000),
                                    MakeEvent("b", 10, 1000)};
  index1.UpdateFromEvents("ctx", events_same, true, half_life);
  float score_same = index1.GetScore("a", "b");

  // One event at time 0, one at time 100 (one half-life apart)
  // max_ts = 1100, event a age = 100, event b age = 0
  // decay_a = exp2(-100/100) = 0.5, decay_b = exp2(0) = 1.0
  // score = 10*10 * 0.5 * 1.0 = 50
  std::vector<Event> events_apart = {MakeEvent("c", 10, 1000),
                                     MakeEvent("d", 10, 1100)};
  index2.UpdateFromEvents("ctx", events_apart, true, half_life);
  float score_apart = index2.GetScore("c", "d");

  // score_apart should be ~half of score_same (50 vs 100)
  EXPECT_NEAR(score_apart / score_same, 0.5, 0.01);
}

TEST(CoOccurrenceTemporalTest, VeryOldEventsNearZero) {
  CoOccurrenceIndex index;

  double half_life = 10.0;  // 10 seconds
  // Event a at time 0, event b at time 1000 (100 half-lives apart)
  std::vector<Event> events = {MakeEvent("a", 10, 0),
                               MakeEvent("b", 10, 1000)};

  index.UpdateFromEvents("ctx", events, true, half_life);

  float score = index.GetScore("a", "b");
  // After 100 half-lives, decay factor for event a is ~2^-100 ≈ 0
  // But clamped to 1e-6
  EXPECT_GT(score, 0.0F);
  EXPECT_LT(score, 0.01F);
}

TEST(CoOccurrenceTemporalTest, SameTimestampNoDecay) {
  CoOccurrenceIndex index1;
  CoOccurrenceIndex index2;

  // All same timestamps
  std::vector<Event> events = {MakeEvent("a", 10, 5000),
                               MakeEvent("b", 10, 5000)};

  index1.UpdateFromEvents("ctx", events, true, 86400.0);
  index2.UpdateFromEvents("ctx", events, false, 0.0);

  // Should produce identical scores (age = 0 for all, decay = 1.0)
  EXPECT_FLOAT_EQ(index1.GetScore("a", "b"), index2.GetScore("a", "b"));
}

}  // namespace nvecd::events
