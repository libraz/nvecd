/**
 * @file similarity_engine_compaction_test.cpp
 * @brief A delete that compacts the store must leave the ANN index consistent
 *        without the caller having to detect the compaction
 */

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include "events/co_occurrence_index.h"
#include "events/event_store.h"
#include "similarity/similarity_engine.h"
#include "vectors/vector_store.h"

namespace nvecd::similarity {
namespace {

class SimilarityEngineCompactionTest : public ::testing::TestWithParam<const char*> {
 protected:
  void SetUp() override {
    events_config_.ctx_buffer_size = 50;
    vectors_config_.default_dimension = 3;
    vectors_config_.distance_metric = "cosine";
    similarity_config_.index_type = GetParam();
    similarity_config_.hnsw_m = 8;
    similarity_config_.hnsw_ef_construction = 32;
    similarity_config_.hnsw_ef_search = 32;
    similarity_config_.max_top_k = 1000;

    event_store_ = std::make_unique<events::EventStore>(events_config_);
    co_index_ = std::make_unique<events::CoOccurrenceIndex>();
    store_ = std::make_unique<vectors::VectorStore>(vectors_config_);
    engine_ = std::make_unique<SimilarityEngine>(event_store_.get(), co_index_.get(), store_.get(), similarity_config_,
                                                 vectors_config_);
  }

  config::EventsConfig events_config_;
  config::VectorsConfig vectors_config_;
  config::SimilarityConfig similarity_config_;
  std::unique_ptr<events::EventStore> event_store_;
  std::unique_ptr<events::CoOccurrenceIndex> co_index_;
  std::unique_ptr<vectors::VectorStore> store_;
  std::unique_ptr<SimilarityEngine> engine_;
};

// Notifying only the single removed label after a compacting delete must not
// leave the index mapping old labels onto re-keyed rows.
TEST_P(SimilarityEngineCompactionTest, NotifyAfterCompactingDeleteRebuildsFromStore) {
  for (int i = 0; i < 8; ++i) {
    const float f = static_cast<float>(i);
    ASSERT_TRUE(store_->SetVector("item" + std::to_string(i), {1.0F, f, 0.5F * f}).has_value());
  }
  engine_->RebuildAnnFromStore();

  for (const char* id : {"item1", "item3", "item5"}) {
    const auto compact_index = store_->GetCompactIndex(id);
    ASSERT_TRUE(compact_index.has_value());
    ASSERT_TRUE(store_->DeleteVector(id));
    engine_->NotifyVectorRemoved(*compact_index);
  }

  EXPECT_EQ(engine_->GetAnnGeneration(), store_->GetGeneration());

  auto results = engine_->SearchByVector({1.0F, 6.0F, 3.0F}, 8);
  ASSERT_TRUE(results.has_value());
  std::unordered_set<std::string> ids;
  for (const auto& result : *results) {
    ids.insert(result.item_id);
  }
  EXPECT_EQ(ids.count("item1"), 0U);
  EXPECT_EQ(ids.count("item3"), 0U);
  EXPECT_EQ(ids.count("item5"), 0U);
  ASSERT_FALSE(results->empty());
  EXPECT_EQ(results->front().item_id, "item6");
  EXPECT_NEAR(results->front().score, 1.0F, 1e-4F);
}

INSTANTIATE_TEST_SUITE_P(AnnIndexTypes, SimilarityEngineCompactionTest, ::testing::Values("hnsw", "ivf"));

}  // namespace
}  // namespace nvecd::similarity
