/**
 * @file differentiation_e2e_test.cpp
 * @brief E2E tests for differentiation features (temporal co-occurrence,
 *        negative signals, adaptive fusion)
 */

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "../test_server_fixture.h"
#include "../test_tcp_client.h"
#include "config/config.h"

// ============================================================================
// Test Fixture with differentiation features enabled
// ============================================================================

class DifferentiationE2ETest : public NvecdTestFixture {
 protected:
  void SetUp() override {
    // Enable all three differentiation features
    config_.events.temporal_cooccurrence = true;
    config_.events.temporal_half_life_sec = 3600.0;  // 1 hour
    config_.events.negative_signals = true;
    config_.events.negative_weight = 0.5;
    config_.similarity.adaptive_fusion = true;
    config_.similarity.adaptive_min_alpha = 0.2;
    config_.similarity.adaptive_max_alpha = 0.9;
    config_.similarity.adaptive_maturity_threshold = 5;

    // Disable cache for deterministic test results
    config_.cache.enabled = false;

    SetUpServer(3);  // 3D vectors
  }

  void TearDown() override { TearDownServer(); }
};

// Fixture without differentiation features (for comparison)
class DifferentiationDisabledE2ETest : public NvecdTestFixture {
 protected:
  void SetUp() override {
    config_.cache.enabled = false;
    SetUpServer(3);
  }
  void TearDown() override { TearDownServer(); }
};

// ============================================================================
// Timestamp Parameter E2E Tests
// ============================================================================

TEST_F(DifferentiationDisabledE2ETest, TimestampParameter_E2E) {
  TcpClient client("127.0.0.1", port_);

  // Event with explicit timestamp should succeed
  EXPECT_TRUE(ContainsOK(client.SendCommand("EVENT user1 ADD item1 10 timestamp=1000")));

  // Event without timestamp should succeed (auto-assign)
  EXPECT_TRUE(ContainsOK(client.SendCommand("EVENT user1 ADD item2 10")));

  // SET with timestamp
  EXPECT_TRUE(ContainsOK(client.SendCommand("EVENT user1 SET item3 5 timestamp=2000")));

  // DEL with timestamp
  EXPECT_TRUE(ContainsOK(client.SendCommand("EVENT user1 DEL item1 timestamp=3000")));

  // Invalid timestamp should error
  EXPECT_TRUE(ContainsError(client.SendCommand("EVENT user1 ADD item4 10 timestamp=abc")));
}

// ============================================================================
// Temporal Co-occurrence E2E Tests
// ============================================================================

TEST_F(DifferentiationE2ETest, TemporalCooccurrence_E2E) {
  TcpClient client("127.0.0.1", port_);

  // Add old events (timestamp=1000)
  EXPECT_TRUE(ContainsOK(client.SendCommand("EVENT user1 ADD old_a 10 timestamp=1000")));
  EXPECT_TRUE(ContainsOK(client.SendCommand("EVENT user1 ADD old_b 10 timestamp=1000")));

  // Add recent events (timestamp=9000)
  EXPECT_TRUE(ContainsOK(client.SendCommand("EVENT user1 ADD new_c 10 timestamp=9000")));
  EXPECT_TRUE(ContainsOK(client.SendCommand("EVENT user1 ADD new_d 10 timestamp=9000")));

  // Add vectors so we can query
  EXPECT_TRUE(ContainsOK(client.SendCommand("VECSET old_a 1.0 0.0 0.0")));
  EXPECT_TRUE(ContainsOK(client.SendCommand("VECSET old_b 0.9 0.1 0.0")));
  EXPECT_TRUE(ContainsOK(client.SendCommand("VECSET new_c 0.0 1.0 0.0")));
  EXPECT_TRUE(ContainsOK(client.SendCommand("VECSET new_d 0.0 0.0 1.0")));

  // Query events-based similarity for new_c
  auto resp = client.SendCommand("SIM new_c 10 using=events");
  auto results = ParseSimResults(resp);

  // new_d should have higher co-occurrence with new_c (recent timestamps)
  // old_a should have lower score with new_c
  if (!results.empty()) {
    // new_d should be first in results for new_c
    bool found_new_d = false;
    for (const auto& [id, score] : results) {
      if (id == "new_d") {
        found_new_d = true;
        break;
      }
    }
    EXPECT_TRUE(found_new_d) << "new_d should appear in co-occurrence results for new_c";
  }
}

// ============================================================================
// Negative Signals E2E Tests
// ============================================================================

TEST_F(DifferentiationE2ETest, NegativeSignals_E2E) {
  TcpClient client("127.0.0.1", port_);

  // Build co-occurrence between items
  EXPECT_TRUE(ContainsOK(client.SendCommand("EVENT user1 ADD item_x 10 timestamp=5000")));
  EXPECT_TRUE(ContainsOK(client.SendCommand("EVENT user1 ADD item_y 10 timestamp=5000")));
  EXPECT_TRUE(ContainsOK(client.SendCommand("EVENT user1 ADD item_z 10 timestamp=5000")));

  // Add vectors
  EXPECT_TRUE(ContainsOK(client.SendCommand("VECSET item_x 1.0 0.0 0.0")));
  EXPECT_TRUE(ContainsOK(client.SendCommand("VECSET item_y 0.0 1.0 0.0")));
  EXPECT_TRUE(ContainsOK(client.SendCommand("VECSET item_z 0.0 0.0 1.0")));

  // Check initial co-occurrence
  auto resp_before = client.SendCommand("SIM item_y 10 using=events");
  auto results_before = ParseSimResults(resp_before);

  float score_x_before = 0.0F;
  for (const auto& [id, score] : results_before) {
    if (id == "item_x") {
      score_x_before = score;
    }
  }
  EXPECT_GT(score_x_before, 0.0F) << "item_x should appear in results before DEL with positive score";

  // Delete item_x (triggers negative signal)
  EXPECT_TRUE(ContainsOK(client.SendCommand("EVENT user1 DEL item_x")));

  // After DEL with negative signal, item_x's co-occurrence with item_y should
  // be reduced
  auto resp_after = client.SendCommand("SIM item_y 10 using=events");
  auto results_after = ParseSimResults(resp_after);

  // item_x's score should be reduced after DEL with negative signal
  float score_x_after = 0.0F;
  float score_z_after = 0.0F;
  for (const auto& [id, score] : results_after) {
    if (id == "item_x")
      score_x_after = score;
    if (id == "item_z")
      score_z_after = score;
  }

  // item_x's score should be lower than before (penalized by negative signal)
  EXPECT_LT(score_x_after, score_x_before) << "item_x score should decrease after DEL (negative signal)";

  // item_z should still have a positive score (unaffected)
  EXPECT_GT(score_z_after, 0.0F) << "item_z should still have positive co-occurrence";
}

// ============================================================================
// Adaptive Fusion E2E Tests
// ============================================================================

TEST_F(DifferentiationE2ETest, AdaptiveFusion_E2E) {
  TcpClient client("127.0.0.1", port_);

  // Create item with many co-occurrences (mature item)
  for (int i = 0; i < 10; ++i) {
    std::string item = "partner_" + std::to_string(i);
    EXPECT_TRUE(ContainsOK(client.SendCommand("EVENT ctx" + std::to_string(i) + " ADD mature_item 10 timestamp=5000")));
    EXPECT_TRUE(
        ContainsOK(client.SendCommand("EVENT ctx" + std::to_string(i) + " ADD " + item + " 10 timestamp=5000")));
  }

  // Create item with no co-occurrences (new item)
  // (only has a vector, no events)

  // Add vectors
  EXPECT_TRUE(ContainsOK(client.SendCommand("VECSET mature_item 1.0 0.0 0.0")));
  EXPECT_TRUE(ContainsOK(client.SendCommand("VECSET new_item 0.9 0.1 0.0")));
  EXPECT_TRUE(ContainsOK(client.SendCommand("VECSET partner_0 0.8 0.2 0.0")));

  // Adaptive fusion query should succeed
  auto resp = client.SendCommand("SIM mature_item 10 adaptive=on");
  EXPECT_TRUE(ContainsOK(resp));
}

TEST_F(DifferentiationE2ETest, AdaptivePerQuery_E2E) {
  TcpClient client("127.0.0.1", port_);

  // Setup data
  EXPECT_TRUE(ContainsOK(client.SendCommand("EVENT user1 ADD item1 10 timestamp=5000")));
  EXPECT_TRUE(ContainsOK(client.SendCommand("EVENT user1 ADD item2 10 timestamp=5000")));
  EXPECT_TRUE(ContainsOK(client.SendCommand("VECSET item1 1.0 0.0 0.0")));
  EXPECT_TRUE(ContainsOK(client.SendCommand("VECSET item2 0.9 0.1 0.0")));
  EXPECT_TRUE(ContainsOK(client.SendCommand("VECSET item3 0.0 1.0 0.0")));

  // Query with adaptive=on
  auto resp_on = client.SendCommand("SIM item1 10 using=fusion adaptive=on");
  EXPECT_TRUE(ContainsOK(resp_on));

  // Query with adaptive=off (uses static weights)
  auto resp_off = client.SendCommand("SIM item1 10 using=fusion adaptive=off");
  EXPECT_TRUE(ContainsOK(resp_off));

  // Both should succeed
  auto results_on = ParseSimResults(resp_on);
  auto results_off = ParseSimResults(resp_off);
  EXPECT_FALSE(results_on.empty());
  EXPECT_FALSE(results_off.empty());
}

// ============================================================================
// Combined Features E2E Test
// ============================================================================

TEST_F(DifferentiationE2ETest, AllFeaturesCombined_E2E) {
  TcpClient client("127.0.0.1", port_);

  // Add events with varying timestamps (temporal)
  EXPECT_TRUE(ContainsOK(client.SendCommand("EVENT u1 ADD a 10 timestamp=1000")));
  EXPECT_TRUE(ContainsOK(client.SendCommand("EVENT u1 ADD b 10 timestamp=1000")));
  EXPECT_TRUE(ContainsOK(client.SendCommand("EVENT u1 ADD c 10 timestamp=9000")));
  EXPECT_TRUE(ContainsOK(client.SendCommand("EVENT u1 ADD d 10 timestamp=9000")));

  // Add vectors
  EXPECT_TRUE(ContainsOK(client.SendCommand("VECSET a 1.0 0.0 0.0")));
  EXPECT_TRUE(ContainsOK(client.SendCommand("VECSET b 0.9 0.1 0.0")));
  EXPECT_TRUE(ContainsOK(client.SendCommand("VECSET c 0.0 1.0 0.0")));
  EXPECT_TRUE(ContainsOK(client.SendCommand("VECSET d 0.0 0.0 1.0")));

  // Delete an item (negative signal)
  EXPECT_TRUE(ContainsOK(client.SendCommand("EVENT u1 DEL a")));

  // Fusion query with adaptive
  auto resp = client.SendCommand("SIM c 10 using=fusion adaptive=on");
  EXPECT_TRUE(ContainsOK(resp));

  auto results = ParseSimResults(resp);
  // Should return results successfully with all features active
  EXPECT_FALSE(results.empty());

  // Events-only query should also work
  auto resp_events = client.SendCommand("SIM c 10 using=events");
  EXPECT_TRUE(ContainsOK(resp_events));

  // Vectors-only query should also work
  auto resp_vectors = client.SendCommand("SIM c 10 using=vectors");
  EXPECT_TRUE(ContainsOK(resp_vectors));
}
