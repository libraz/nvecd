/**
 * @file adaptive_fusion_test.cpp
 * @brief Unit tests for adaptive fusion feature
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <memory>

#include "config/config.h"
#include "events/co_occurrence_index.h"
#include "events/event_store.h"
#include "similarity/similarity_engine.h"
#include "vectors/vector_store.h"

namespace nvecd::similarity {

class AdaptiveFusionTest : public ::testing::Test {
 protected:
  void SetUp() override {
    events_config_.ctx_buffer_size = 100;
    events_config_.decay_alpha = 0.99;
    events_config_.decay_interval_sec = 3600;

    vectors_config_.default_dimension = 3;
    vectors_config_.distance_metric = "cosine";

    sim_config_.default_top_k = 10;
    sim_config_.max_top_k = 100;
    sim_config_.fusion_alpha = 0.5;
    sim_config_.fusion_beta = 0.5;
    sim_config_.adaptive_fusion = false;
    sim_config_.adaptive_min_alpha = 0.2;
    sim_config_.adaptive_max_alpha = 0.9;
    sim_config_.adaptive_maturity_threshold = 10;

    event_store_ = std::make_unique<events::EventStore>(events_config_);
    co_index_ = std::make_unique<events::CoOccurrenceIndex>();
    vector_store_ = std::make_unique<vectors::VectorStore>(vectors_config_);
  }

  void CreateEngine() {
    engine_ = std::make_unique<SimilarityEngine>(event_store_.get(), co_index_.get(), vector_store_.get(), sim_config_,
                                                 vectors_config_);
  }

  void PopulateData() {
    // Add vectors for all items
    vector_store_->SetVector("item1", {1.0F, 0.0F, 0.0F});
    vector_store_->SetVector("item2", {0.9F, 0.1F, 0.0F});
    vector_store_->SetVector("item3", {0.0F, 1.0F, 0.0F});
    vector_store_->SetVector("item4", {0.0F, 0.0F, 1.0F});

    // Add many events for item1 to make it "mature"
    for (int i = 0; i < 20; ++i) {
      std::string other = "item" + std::to_string(i + 10);
      co_index_->SetScore("item1", other, static_cast<float>(100 - i));
    }

    // item4 has no co-occurrence data (new item)
  }

  config::EventsConfig events_config_;
  config::VectorsConfig vectors_config_;
  config::SimilarityConfig sim_config_;
  std::unique_ptr<events::EventStore> event_store_;
  std::unique_ptr<events::CoOccurrenceIndex> co_index_;
  std::unique_ptr<vectors::VectorStore> vector_store_;
  std::unique_ptr<SimilarityEngine> engine_;
};

TEST_F(AdaptiveFusionTest, AdaptiveDisabledByDefault) {
  sim_config_.adaptive_fusion = false;
  PopulateData();
  CreateEngine();

  // When adaptive is disabled, uses static alpha/beta from config
  auto result = engine_->SearchByIdFusion("item1", 10);
  ASSERT_TRUE(result.has_value());
  // Just verify it returns results without error
  EXPECT_FALSE(result->empty());
}

TEST_F(AdaptiveFusionTest, StaticFusionUsesConfiguredWeightedNormalizedScores) {
  sim_config_.fusion_alpha = 0.25;
  sim_config_.fusion_beta = 0.75;
  vector_store_->SetVector("query", {1.0F, 0.0F, 0.0F});
  // Candidate wins events but loses vector similarity after per-source
  // normalization; competitor has the inverse signal.
  vector_store_->SetVector("event_winner", {0.8F, 0.6F, 0.0F});
  vector_store_->SetVector("vector_winner", {1.0F, 0.0F, 0.0F});
  co_index_->SetScore("query", "event_winner", 10.0F);
  co_index_->SetScore("query", "vector_winner", 0.0F);
  CreateEngine();

  auto results = engine_->SearchByIdFusion("query", 2, false);
  ASSERT_TRUE(results.has_value());
  ASSERT_EQ(results->size(), 2U);

  const auto event_winner = std::find_if(results->begin(), results->end(),
                                         [](const auto& result) { return result.item_id == "event_winner"; });
  const auto vector_winner = std::find_if(results->begin(), results->end(),
                                          [](const auto& result) { return result.item_id == "vector_winner"; });
  ASSERT_NE(event_winner, results->end());
  ASSERT_NE(vector_winner, results->end());
  EXPECT_NEAR(event_winner->score, 0.75F, 1e-5F);
  EXPECT_NEAR(vector_winner->score, 0.25F, 1e-5F);
}

TEST_F(AdaptiveFusionTest, GetNeighborCount) {
  co_index_->SetScore("a", "b", 1.0F);
  co_index_->SetScore("a", "c", 2.0F);
  co_index_->SetScore("a", "d", 3.0F);

  EXPECT_EQ(co_index_->GetNeighborCount("a"), 3U);
  EXPECT_EQ(co_index_->GetNeighborCount("b"), 1U);
  EXPECT_EQ(co_index_->GetNeighborCount("nonexistent"), 0U);
}

TEST_F(AdaptiveFusionTest, PerQueryOverride) {
  sim_config_.adaptive_fusion = false;  // Disabled in config
  PopulateData();
  CreateEngine();

  // Override with adaptive=true at query time
  auto result_adaptive = engine_->SearchByIdFusion("item1", 10, true);
  ASSERT_TRUE(result_adaptive.has_value());

  // Override with adaptive=false at query time
  auto result_static = engine_->SearchByIdFusion("item1", 10, false);
  ASSERT_TRUE(result_static.has_value());

  // Both should succeed, and the mature item must use different weights when
  // adaptive fusion is explicitly enabled. This checks the observable score
  // rather than merely exercising the optional parameter.
  EXPECT_FALSE(result_adaptive->empty());
  EXPECT_FALSE(result_static->empty());

  bool saw_changed_score = false;
  for (const auto& static_result : *result_static) {
    for (const auto& adaptive_result : *result_adaptive) {
      if (static_result.item_id == adaptive_result.item_id &&
          std::abs(static_result.score - adaptive_result.score) > 0.0001F) {
        saw_changed_score = true;
        break;
      }
    }
    if (saw_changed_score) {
      break;
    }
  }
  EXPECT_TRUE(saw_changed_score);
}

TEST_F(AdaptiveFusionTest, GenerationCounter) {
  uint64_t gen0 = co_index_->GetGeneration();

  co_index_->SetScore("a", "b", 1.0F);
  uint64_t gen1 = co_index_->GetGeneration();
  EXPECT_GT(gen1, gen0);

  std::vector<events::Event> events = {events::Event("c", 10, 1000), events::Event("d", 10, 1000)};
  co_index_->UpdateFromEvents("ctx", events);
  uint64_t gen2 = co_index_->GetGeneration();
  EXPECT_GT(gen2, gen1);

  co_index_->Clear();
  uint64_t gen3 = co_index_->GetGeneration();
  EXPECT_GT(gen3, gen2);
}

}  // namespace nvecd::similarity
