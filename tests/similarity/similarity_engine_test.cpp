/**
 * @file similarity_engine_test.cpp
 * @brief Unit tests for SimilarityEngine
 */

#include "similarity/similarity_engine.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <unordered_set>
#include <vector>

#include "events/co_occurrence_index.h"
#include "events/event_store.h"
#include "vectors/metadata_filter.h"
#include "vectors/metadata_store.h"
#include "vectors/vector_store.h"

namespace nvecd::similarity {
namespace {

// Helper to create configs
config::EventsConfig MakeEventsConfig() {
  config::EventsConfig config;
  config.ctx_buffer_size = 50;
  config.decay_interval_sec = 3600;
  config.decay_alpha = 0.99;
  return config;
}

config::VectorsConfig MakeVectorsConfig() {
  config::VectorsConfig config;
  config.default_dimension = 3;
  config.distance_metric = "cosine";
  return config;
}

config::SimilarityConfig MakeSimilarityConfig() {
  config::SimilarityConfig config;
  config.default_top_k = 100;
  config.max_top_k = 1000;
  config.fusion_alpha = 0.6;
  config.fusion_beta = 0.4;
  return config;
}

// Test fixture
class SimilarityEngineTest : public ::testing::Test {
 protected:
  void SetUp() override {
    events_config_ = MakeEventsConfig();
    vectors_config_ = MakeVectorsConfig();
    similarity_config_ = MakeSimilarityConfig();

    event_store_ = std::make_unique<events::EventStore>(events_config_);
    co_index_ = std::make_unique<events::CoOccurrenceIndex>();
    vector_store_ = std::make_unique<vectors::VectorStore>(vectors_config_);

    engine_ = std::make_unique<SimilarityEngine>(event_store_.get(), co_index_.get(), vector_store_.get(),
                                                 similarity_config_, vectors_config_);
  }

  config::EventsConfig events_config_;
  config::VectorsConfig vectors_config_;
  config::SimilarityConfig similarity_config_;

  std::unique_ptr<events::EventStore> event_store_;
  std::unique_ptr<events::CoOccurrenceIndex> co_index_;
  std::unique_ptr<vectors::VectorStore> vector_store_;
  std::unique_ptr<SimilarityEngine> engine_;
};

// ============================================================================
// Events-based Search Tests
// ============================================================================

TEST_F(SimilarityEngineTest, SearchByIdEvents_Empty) {
  auto results = engine_->SearchByIdEvents("item1", 10);
  ASSERT_TRUE(results.has_value());
  EXPECT_TRUE(results->empty());
}

TEST_F(SimilarityEngineTest, SearchByIdEvents_WithCoOccurrence) {
  // Add events to create co-occurrences
  std::vector<events::Event> events = {{"item1", 10, 1000}, {"item2", 20, 1001}, {"item3", 15, 1002}};

  co_index_->UpdateFromEvents("ctx1", events);

  auto results = engine_->SearchByIdEvents("item1", 10);
  ASSERT_TRUE(results.has_value());
  EXPECT_GT(results->size(), 0);

  // Should have item2 and item3 as similar items
  EXPECT_EQ(results->size(), 2);
}

TEST_F(SimilarityEngineTest, SearchByIdEvents_TopK) {
  // Create many co-occurrences
  std::vector<events::Event> events;
  for (int i = 1; i <= 20; ++i) {
    events.emplace_back("item" + std::to_string(i), i, 1000 + i);
  }

  co_index_->UpdateFromEvents("ctx1", events);

  auto results = engine_->SearchByIdEvents("item1", 5);
  ASSERT_TRUE(results.has_value());
  EXPECT_LE(results->size(), 5);
}

TEST_F(SimilarityEngineTest, SearchByIdEvents_InvalidTopK) {
  auto result1 = engine_->SearchByIdEvents("item1", 0);
  EXPECT_FALSE(result1.has_value());
  EXPECT_EQ(result1.error().code(), utils::ErrorCode::kCommandInvalidTopK);

  auto result2 = engine_->SearchByIdEvents("item1", -1);
  EXPECT_FALSE(result2.has_value());
  EXPECT_EQ(result2.error().code(), utils::ErrorCode::kCommandInvalidTopK);

  auto result3 = engine_->SearchByIdEvents("item1", 10000);
  EXPECT_FALSE(result3.has_value());
  EXPECT_EQ(result3.error().code(), utils::ErrorCode::kCommandInvalidTopK);
}

// ============================================================================
// Vectors-based Search Tests
// ============================================================================

TEST_F(SimilarityEngineTest, SearchByIdVectors_NotFound) {
  auto results = engine_->SearchByIdVectors("nonexistent", 10);
  ASSERT_FALSE(results.has_value());
  EXPECT_EQ(results.error().code(), utils::ErrorCode::kVectorNotFound);
}

TEST_F(SimilarityEngineTest, SearchByIdVectors_WithVectors) {
  // Add vectors
  ASSERT_TRUE(vector_store_->SetVector("item1", {0.1f, 0.2f, 0.3f}).has_value());
  ASSERT_TRUE(vector_store_->SetVector("item2", {0.15f, 0.25f, 0.35f}).has_value());
  ASSERT_TRUE(vector_store_->SetVector("item3", {0.9f, 0.8f, 0.7f}).has_value());

  auto results = engine_->SearchByIdVectors("item1", 10);
  ASSERT_TRUE(results.has_value());
  EXPECT_GT(results->size(), 0);

  // item2 should be more similar than item3
  if (results->size() >= 2) {
    EXPECT_EQ((*results)[0].item_id, "item2");
  }
}

TEST_F(SimilarityEngineTest, SearchByIdVectors_SortedByScore) {
  // Add vectors with known similarities
  ASSERT_TRUE(vector_store_->SetVector("item1", {1.0f, 0.0f, 0.0f}).has_value());
  ASSERT_TRUE(vector_store_->SetVector("item2", {0.9f, 0.1f, 0.0f}).has_value());   // Very similar
  ASSERT_TRUE(vector_store_->SetVector("item3", {0.0f, 1.0f, 0.0f}).has_value());   // Orthogonal
  ASSERT_TRUE(vector_store_->SetVector("item4", {-1.0f, 0.0f, 0.0f}).has_value());  // Opposite

  auto results = engine_->SearchByIdVectors("item1", 10);
  ASSERT_TRUE(results.has_value());
  ASSERT_GE(results->size(), 3);

  // Results should be sorted by score descending
  for (size_t i = 1; i < results->size(); ++i) {
    EXPECT_GE((*results)[i - 1].score, (*results)[i].score);
  }

  // item2 should be most similar
  EXPECT_EQ((*results)[0].item_id, "item2");
}

// ============================================================================
// Fusion Search Tests
// ============================================================================

TEST_F(SimilarityEngineTest, SearchByIdFusion_BothEmpty) {
  auto results = engine_->SearchByIdFusion("item1", 10);
  ASSERT_FALSE(results.has_value());
  EXPECT_EQ(results.error().code(), utils::ErrorCode::kVectorNotFound);
}

TEST_F(SimilarityEngineTest, SearchByIdFusion_OnlyEvents) {
  // Add events
  std::vector<events::Event> events = {{"item1", 10, 1000}, {"item2", 20, 1001}, {"item3", 15, 1002}};
  co_index_->UpdateFromEvents("ctx1", events);

  auto results = engine_->SearchByIdFusion("item1", 10);
  ASSERT_TRUE(results.has_value());
  EXPECT_GT(results->size(), 0);
}

TEST_F(SimilarityEngineTest, SearchByIdFusion_OnlyVectors) {
  // Add vectors
  ASSERT_TRUE(vector_store_->SetVector("item1", {0.1f, 0.2f, 0.3f}).has_value());
  ASSERT_TRUE(vector_store_->SetVector("item2", {0.15f, 0.25f, 0.35f}).has_value());

  auto results = engine_->SearchByIdFusion("item1", 10);
  ASSERT_TRUE(results.has_value());
  EXPECT_GT(results->size(), 0);
}

TEST_F(SimilarityEngineTest, SearchByIdFusion_BothSources) {
  // Add events
  std::vector<events::Event> events = {{"item1", 10, 1000}, {"item2", 20, 1001}, {"item3", 15, 1002}};
  co_index_->UpdateFromEvents("ctx1", events);

  // Add vectors
  ASSERT_TRUE(vector_store_->SetVector("item1", {0.1f, 0.2f, 0.3f}).has_value());
  ASSERT_TRUE(vector_store_->SetVector("item2", {0.15f, 0.25f, 0.35f}).has_value());
  ASSERT_TRUE(vector_store_->SetVector("item3", {0.9f, 0.8f, 0.7f}).has_value());

  auto results = engine_->SearchByIdFusion("item1", 10);
  ASSERT_TRUE(results.has_value());
  EXPECT_GT(results->size(), 0);

  // Should combine both sources
  EXPECT_GE(results->size(), 2);
}

// ============================================================================
// Vector Query Search (SIMV) Tests
// ============================================================================

TEST_F(SimilarityEngineTest, SearchByVector_EmptyQuery) {
  std::vector<float> empty_vec;
  auto results = engine_->SearchByVector(empty_vec, 10);
  ASSERT_FALSE(results.has_value());
  EXPECT_EQ(results.error().code(), utils::ErrorCode::kInvalidArgument);
}

TEST_F(SimilarityEngineTest, SearchByVector_DimensionMismatch) {
  // Add a 3D vector to establish dimension
  ASSERT_TRUE(vector_store_->SetVector("item1", {0.1f, 0.2f, 0.3f}).has_value());

  // Try to search with 2D vector
  std::vector<float> query = {0.1f, 0.2f};
  auto results = engine_->SearchByVector(query, 10);
  ASSERT_FALSE(results.has_value());
  EXPECT_EQ(results.error().code(), utils::ErrorCode::kVectorDimensionMismatch);
}

TEST_F(SimilarityEngineTest, SearchByVector_ValidQuery) {
  // Add vectors
  ASSERT_TRUE(vector_store_->SetVector("item1", {0.1f, 0.2f, 0.3f}).has_value());
  ASSERT_TRUE(vector_store_->SetVector("item2", {0.15f, 0.25f, 0.35f}).has_value());
  ASSERT_TRUE(vector_store_->SetVector("item3", {0.9f, 0.8f, 0.7f}).has_value());

  // Query with vector similar to item1
  std::vector<float> query = {0.12f, 0.22f, 0.32f};
  auto results = engine_->SearchByVector(query, 10);
  ASSERT_TRUE(results.has_value());
  EXPECT_GT(results->size(), 0);

  // Should find all items
  EXPECT_EQ(results->size(), 3);

  // Results should be sorted
  for (size_t i = 1; i < results->size(); ++i) {
    EXPECT_GE((*results)[i - 1].score, (*results)[i].score);
  }
}

TEST_F(SimilarityEngineTest, SearchByVector_TopK) {
  // Add many vectors
  for (int i = 0; i < 20; ++i) {
    float val = static_cast<float>(i) / 20.0f;
    ASSERT_TRUE(vector_store_->SetVector("item" + std::to_string(i), {val, val, val}).has_value());
  }

  std::vector<float> query = {0.5f, 0.5f, 0.5f};
  auto results = engine_->SearchByVector(query, 5);
  ASSERT_TRUE(results.has_value());
  EXPECT_LE(results->size(), 5);
}

// A public VECDEL compacts VectorStore then rebuilds ANN indexes because their
// labels are compact row indices.  This regression test proves a HNSW rebuild
// maps the remaining rows back to their current IDs after the deleted middle
// row shifts the final item's compact index.
TEST(SimilarityEngineAnnDeleteTest, HnswRebuildAfterDefragmentDoesNotReturnDeletedId) {
  auto events_config = MakeEventsConfig();
  auto vectors_config = MakeVectorsConfig();
  auto similarity_config = MakeSimilarityConfig();
  similarity_config.index_type = "hnsw";
  similarity_config.hnsw_m = 8;
  similarity_config.hnsw_ef_construction = 32;
  similarity_config.hnsw_ef_search = 16;

  events::EventStore event_store(events_config);
  events::CoOccurrenceIndex co_index;
  vectors::VectorStore vector_store(vectors_config);
  SimilarityEngine engine(&event_store, &co_index, &vector_store, similarity_config, vectors_config);

  ASSERT_TRUE(vector_store.SetVector("first", {1.0F, 0.0F, 0.0F}).has_value());
  ASSERT_TRUE(vector_store.SetVector("deleted", {0.9F, 0.1F, 0.0F}).has_value());
  ASSERT_TRUE(vector_store.SetVector("shifted", {0.0F, 1.0F, 0.0F}).has_value());
  engine.RebuildAnnFromStore();

  const auto deleted_index = vector_store.GetCompactIndex("deleted");
  ASSERT_TRUE(deleted_index.has_value());
  engine.NotifyVectorRemoved(*deleted_index);
  ASSERT_TRUE(vector_store.DeleteVector("deleted"));
  vector_store.Defragment();
  engine.RebuildAnnFromStore();

  auto results = engine.SearchByVector({1.0F, 0.0F, 0.0F}, 10);
  ASSERT_TRUE(results.has_value());
  std::unordered_set<std::string> ids;
  for (const auto& result : *results) {
    ids.insert(result.item_id);
  }
  EXPECT_NE(ids.find("first"), ids.end());
  EXPECT_NE(ids.find("shifted"), ids.end());
  EXPECT_EQ(ids.find("deleted"), ids.end());
}

// ============================================================================
// Edge Cases
// ============================================================================

TEST_F(SimilarityEngineTest, TopKLargerThanResults) {
  // Add only 3 vectors
  ASSERT_TRUE(vector_store_->SetVector("item1", {0.1f, 0.2f, 0.3f}).has_value());
  ASSERT_TRUE(vector_store_->SetVector("item2", {0.15f, 0.25f, 0.35f}).has_value());
  ASSERT_TRUE(vector_store_->SetVector("item3", {0.9f, 0.8f, 0.7f}).has_value());

  // Request more than available
  auto results = engine_->SearchByIdVectors("item1", 100);
  ASSERT_TRUE(results.has_value());
  EXPECT_EQ(results->size(), 2);  // Excludes self
}

TEST_F(SimilarityEngineTest, ScoresAreDescending) {
  // Add vectors
  ASSERT_TRUE(vector_store_->SetVector("item1", {1.0f, 0.0f, 0.0f}).has_value());
  ASSERT_TRUE(vector_store_->SetVector("item2", {0.9f, 0.1f, 0.0f}).has_value());
  ASSERT_TRUE(vector_store_->SetVector("item3", {0.8f, 0.2f, 0.0f}).has_value());
  ASSERT_TRUE(vector_store_->SetVector("item4", {0.7f, 0.3f, 0.0f}).has_value());

  auto results = engine_->SearchByIdVectors("item1", 10);
  ASSERT_TRUE(results.has_value());

  // Verify descending order
  for (size_t i = 1; i < results->size(); ++i) {
    EXPECT_GE((*results)[i - 1].score, (*results)[i].score) << "Results not in descending order at index " << i;
  }
}

TEST_F(SimilarityEngineTest, NoDuplicatesInFusion) {
  // Add events and vectors for same items
  std::vector<events::Event> events = {{"item1", 10, 1000}, {"item2", 20, 1001}, {"item3", 15, 1002}};
  co_index_->UpdateFromEvents("ctx1", events);

  ASSERT_TRUE(vector_store_->SetVector("item1", {0.1f, 0.2f, 0.3f}).has_value());
  ASSERT_TRUE(vector_store_->SetVector("item2", {0.15f, 0.25f, 0.35f}).has_value());
  ASSERT_TRUE(vector_store_->SetVector("item3", {0.9f, 0.8f, 0.7f}).has_value());

  auto results = engine_->SearchByIdFusion("item1", 10);
  ASSERT_TRUE(results.has_value());

  // Check for duplicates
  std::unordered_set<std::string> seen_ids;
  for (const auto& result : *results) {
    EXPECT_EQ(seen_ids.count(result.item_id), 0) << "Duplicate ID: " << result.item_id;
    seen_ids.insert(result.item_id);
  }
}

// ============================================================================
// Fusion Search Parameter Tests
// ============================================================================

TEST_F(SimilarityEngineTest, FusionParameters_AlphaOnly) {
  // Test with alpha=1.0, beta=0.0 (vectors only - alpha weights vectors!)
  config::SimilarityConfig config;
  config.default_top_k = 10;
  config.fusion_alpha = 1.0f;  // alpha weights VECTORS
  config.fusion_beta = 0.0f;   // beta weights EVENTS

  SimilarityEngine engine(event_store_.get(), co_index_.get(), vector_store_.get(), config);

  // Add both events and vectors
  std::vector<events::Event> events = {{"item1", 100, 1000}, {"item2", 50, 1001}, {"item3", 25, 1002}};
  co_index_->UpdateFromEvents("ctx1", events);

  ASSERT_TRUE(vector_store_->SetVector("item1", {0.1f, 0.2f, 0.3f}).has_value());
  ASSERT_TRUE(vector_store_->SetVector("item2", {0.9f, 0.8f, 0.7f}).has_value());
  ASSERT_TRUE(vector_store_->SetVector("item3", {0.15f, 0.25f, 0.35f}).has_value());  // Most similar vector

  auto results = engine.SearchByIdFusion("item1", 10);
  ASSERT_TRUE(results.has_value());
  EXPECT_GT(results->size(), 0);

  // With alpha=1.0, beta=0.0, results should match vectors-only search
  auto vector_results = engine.SearchByIdVectors("item1", 10);
  ASSERT_TRUE(vector_results.has_value());

  // First result should prioritize vector similarity (item3 is most similar)
  if (!vector_results->empty() && !results->empty()) {
    EXPECT_EQ((*results)[0].item_id, (*vector_results)[0].item_id);
  }
}

TEST_F(SimilarityEngineTest, FusionParameters_BetaOnly) {
  // Test with alpha=0.0, beta=1.0 (events only - beta weights events!)
  config::SimilarityConfig config;
  config.default_top_k = 10;
  config.fusion_alpha = 0.0f;  // alpha weights VECTORS
  config.fusion_beta = 1.0f;   // beta weights EVENTS

  SimilarityEngine engine(event_store_.get(), co_index_.get(), vector_store_.get(), config);

  // Add both events and vectors
  std::vector<events::Event> events = {{"item1", 100, 1000}, {"item2", 50, 1001}, {"item3", 25, 1002}};
  co_index_->UpdateFromEvents("ctx1", events);

  ASSERT_TRUE(vector_store_->SetVector("item1", {0.1f, 0.2f, 0.3f}).has_value());
  ASSERT_TRUE(vector_store_->SetVector("item2", {0.9f, 0.8f, 0.7f}).has_value());
  ASSERT_TRUE(vector_store_->SetVector("item3", {0.15f, 0.25f, 0.35f}).has_value());  // Most similar vector

  auto results = engine.SearchByIdFusion("item1", 10);
  ASSERT_TRUE(results.has_value());
  EXPECT_GT(results->size(), 0);

  // With alpha=0.0, beta=1.0, results should match events-only search (beta weights events)
  auto event_results = engine.SearchByIdEvents("item1", 10);
  ASSERT_TRUE(event_results.has_value());

  // First result should prioritize event score (item2 has highest event score: 50)
  if (!event_results->empty() && !results->empty()) {
    EXPECT_EQ((*results)[0].item_id, (*event_results)[0].item_id);
  }
}

TEST_F(SimilarityEngineTest, FusionParameters_Balanced) {
  // Test with alpha=0.5, beta=0.5 (balanced fusion)
  config::SimilarityConfig config;
  config.default_top_k = 10;
  config.fusion_alpha = 0.5f;
  config.fusion_beta = 0.5f;

  SimilarityEngine engine(event_store_.get(), co_index_.get(), vector_store_.get(), config);

  // Add events and vectors
  std::vector<events::Event> events = {
      {"item1", 100, 1000}, {"item2", 80, 1001}, {"item3", 50, 1002}, {"item4", 20, 1003}};
  co_index_->UpdateFromEvents("ctx1", events);

  ASSERT_TRUE(vector_store_->SetVector("item1", {0.1f, 0.2f, 0.3f}).has_value());
  ASSERT_TRUE(vector_store_->SetVector("item2", {0.5f, 0.6f, 0.7f}).has_value());
  ASSERT_TRUE(vector_store_->SetVector("item3", {0.15f, 0.25f, 0.35f}).has_value());  // Similar vector
  ASSERT_TRUE(vector_store_->SetVector("item4", {0.12f, 0.22f, 0.32f}).has_value());  // Very similar vector

  auto results = engine.SearchByIdFusion("item1", 10);
  ASSERT_TRUE(results.has_value());
  EXPECT_GT(results->size(), 0);

  // All results should have combined scores
  for (const auto& result : *results) {
    EXPECT_GT(result.score, 0.0f);
  }
}

TEST_F(SimilarityEngineTest, FusionParameters_AlphaDominant) {
  // Test with alpha=0.8, beta=0.2 (vectors dominant - alpha weights vectors!)
  config::SimilarityConfig config;
  config.default_top_k = 10;
  config.fusion_alpha = 0.8f;  // alpha weights VECTORS
  config.fusion_beta = 0.2f;   // beta weights EVENTS

  SimilarityEngine engine(event_store_.get(), co_index_.get(), vector_store_.get(), config);

  // Item with high event score but low vector similarity
  std::vector<events::Event> events = {
      {"item1", 10, 1000}, {"item_high_event", 100, 1001}, {"item_high_vector", 5, 1002}};
  co_index_->UpdateFromEvents("ctx1", events);

  ASSERT_TRUE(vector_store_->SetVector("item1", {0.1f, 0.2f, 0.3f}).has_value());
  ASSERT_TRUE(vector_store_->SetVector("item_high_event", {0.9f, 0.8f, 0.7f}).has_value());      // Different vector
  ASSERT_TRUE(vector_store_->SetVector("item_high_vector", {0.11f, 0.21f, 0.31f}).has_value());  // Similar vector

  auto results = engine.SearchByIdFusion("item1", 10);
  ASSERT_TRUE(results.has_value());
  ASSERT_GE(results->size(), 2);

  // With alpha=0.8, item_high_vector should rank higher despite poor event score
  bool high_event_found = false;
  bool high_vector_found = false;
  size_t high_event_pos = 0;
  size_t high_vector_pos = 0;

  for (size_t i = 0; i < results->size(); ++i) {
    if ((*results)[i].item_id == "item_high_event") {
      high_event_found = true;
      high_event_pos = i;
    }
    if ((*results)[i].item_id == "item_high_vector") {
      high_vector_found = true;
      high_vector_pos = i;
    }
  }

  if (high_event_found && high_vector_found) {
    EXPECT_LT(high_vector_pos, high_event_pos) << "With alpha=0.8, vector similarity should dominate";
  }
}

TEST_F(SimilarityEngineTest, FusionParameters_BetaDominant) {
  // Test with alpha=0.2, beta=0.8 (events dominant - beta weights events!)
  config::SimilarityConfig config;
  config.default_top_k = 10;
  config.fusion_alpha = 0.2f;  // alpha weights VECTORS
  config.fusion_beta = 0.8f;   // beta weights EVENTS

  SimilarityEngine engine(event_store_.get(), co_index_.get(), vector_store_.get(), config);

  // Item with high vector similarity but low event score
  std::vector<events::Event> events = {
      {"item1", 10, 1000}, {"item_high_event", 100, 1001}, {"item_high_vector", 5, 1002}};
  co_index_->UpdateFromEvents("ctx1", events);

  ASSERT_TRUE(vector_store_->SetVector("item1", {0.1f, 0.2f, 0.3f}).has_value());
  ASSERT_TRUE(vector_store_->SetVector("item_high_event", {0.9f, 0.8f, 0.7f}).has_value());      // Different vector
  ASSERT_TRUE(vector_store_->SetVector("item_high_vector", {0.11f, 0.21f, 0.31f}).has_value());  // Similar vector

  auto results = engine.SearchByIdFusion("item1", 10);
  ASSERT_TRUE(results.has_value());
  ASSERT_GE(results->size(), 2);

  // With beta=0.8, item_high_event should rank higher despite poor vector similarity
  bool high_event_found = false;
  bool high_vector_found = false;
  size_t high_event_pos = 0;
  size_t high_vector_pos = 0;

  for (size_t i = 0; i < results->size(); ++i) {
    if ((*results)[i].item_id == "item_high_event") {
      high_event_found = true;
      high_event_pos = i;
    }
    if ((*results)[i].item_id == "item_high_vector") {
      high_vector_found = true;
      high_vector_pos = i;
    }
  }

  if (high_event_found && high_vector_found) {
    EXPECT_LT(high_event_pos, high_vector_pos) << "With beta=0.8, event score should dominate";
  }
}

// ============================================================================
// Distance Metric Selection Tests
// ============================================================================

TEST_F(SimilarityEngineTest, DistanceMetric_Cosine) {
  // Default uses cosine
  ASSERT_TRUE(vector_store_->SetVector("item1", {1.0f, 0.0f, 0.0f}).has_value());
  ASSERT_TRUE(vector_store_->SetVector("item2", {1.0f, 0.0f, 0.0f}).has_value());  // Identical direction

  auto results = engine_->SearchByIdVectors("item1", 10);
  ASSERT_TRUE(results.has_value());
  ASSERT_EQ(results->size(), 1);
  EXPECT_NEAR((*results)[0].score, 1.0f, 1e-5f);  // Cosine of identical = 1.0
}

TEST_F(SimilarityEngineTest, DistanceMetric_DotProduct) {
  config::VectorsConfig vec_cfg;
  vec_cfg.default_dimension = 3;
  vec_cfg.distance_metric = "dot";

  auto vec_store = std::make_unique<vectors::VectorStore>(vec_cfg);
  ASSERT_TRUE(vec_store->SetVector("item1", {1.0f, 2.0f, 3.0f}).has_value());
  ASSERT_TRUE(vec_store->SetVector("item2", {4.0f, 5.0f, 6.0f}).has_value());

  SimilarityEngine engine(event_store_.get(), co_index_.get(), vec_store.get(), similarity_config_, vec_cfg);

  auto results = engine.SearchByIdVectors("item1", 10);
  ASSERT_TRUE(results.has_value());
  ASSERT_EQ(results->size(), 1);
  // Dot product: 1*4 + 2*5 + 3*6 = 32
  EXPECT_NEAR((*results)[0].score, 32.0f, 1e-3f);
}

TEST_F(SimilarityEngineTest, DistanceMetric_L2) {
  config::VectorsConfig vec_cfg;
  vec_cfg.default_dimension = 3;
  vec_cfg.distance_metric = "l2";

  auto vec_store = std::make_unique<vectors::VectorStore>(vec_cfg);
  ASSERT_TRUE(vec_store->SetVector("item1", {0.0f, 0.0f, 0.0f}).has_value());
  ASSERT_TRUE(vec_store->SetVector("item2", {1.0f, 0.0f, 0.0f}).has_value());
  ASSERT_TRUE(vec_store->SetVector("item3", {10.0f, 0.0f, 0.0f}).has_value());

  SimilarityEngine engine(event_store_.get(), co_index_.get(), vec_store.get(), similarity_config_, vec_cfg);

  auto results = engine.SearchByIdVectors("item1", 10);
  ASSERT_TRUE(results.has_value());
  ASSERT_EQ(results->size(), 2);

  // L2 metric: score = 1/(1+distance), closer items have higher scores
  // item2 is closer to item1 than item3
  EXPECT_EQ((*results)[0].item_id, "item2");
  EXPECT_GT((*results)[0].score, (*results)[1].score);

  // item2: 1/(1+1.0) = 0.5
  EXPECT_NEAR((*results)[0].score, 0.5f, 1e-5f);
}

TEST_F(SimilarityEngineTest, FusionNormalization_SingleResult) {
  // When one source has a single result, NormalizeScores preserves its real
  // similarity clamped to [0, 1] (not a fixed 0.5), so absolute confidence is
  // retained. Fusion scores must stay non-negative and well-formed.

  // Only one co-occurrence pair
  std::vector<events::Event> events = {{"item1", 10, 1000}, {"item2", 20, 1001}};
  co_index_->UpdateFromEvents("ctx1", events);

  // Multiple vectors for contrast
  ASSERT_TRUE(vector_store_->SetVector("item1", {0.1f, 0.2f, 0.3f}).has_value());
  ASSERT_TRUE(vector_store_->SetVector("item2", {0.15f, 0.25f, 0.35f}).has_value());
  ASSERT_TRUE(vector_store_->SetVector("item3", {0.9f, 0.8f, 0.7f}).has_value());
  ASSERT_TRUE(vector_store_->SetVector("item4", {0.12f, 0.22f, 0.32f}).has_value());

  auto results = engine_->SearchByIdFusion("item1", 10);
  ASSERT_TRUE(results.has_value());
  EXPECT_GT(results->size(), 0);

  // All fusion scores should be non-negative
  for (const auto& r : *results) {
    EXPECT_GE(r.score, 0.0f);
  }
}

// ============================================================================
// Metadata Filter Desync Regression (C-1)
// ============================================================================

// Regression test for the MetadataStore desync bug: metadata used to be keyed
// by the VectorStore compact_index, which is unstable across tombstone-slot
// reuse and defragmentation. After re-keying by the stable item ID, deleting a
// vector and reusing its slot must NOT leak the deleted item's metadata onto a
// different item, and the deleted item's metadata must be gone.
class MetadataDesyncTest : public ::testing::Test {
 protected:
  void SetUp() override {
    events_config_ = MakeEventsConfig();
    vectors_config_ = MakeVectorsConfig();
    similarity_config_ = MakeSimilarityConfig();
    // Disable sampling so the brute-force scan visits every candidate.
    similarity_config_.sample_size = 0;

    event_store_ = std::make_unique<events::EventStore>(events_config_);
    co_index_ = std::make_unique<events::CoOccurrenceIndex>();
    vector_store_ = std::make_unique<vectors::VectorStore>(vectors_config_);
    metadata_store_ = std::make_unique<vectors::MetadataStore>();

    engine_ = std::make_unique<SimilarityEngine>(event_store_.get(), co_index_.get(), vector_store_.get(),
                                                 similarity_config_, vectors_config_, metadata_store_.get());
  }

  // Build a filter that matches items whose "owner" field equals the given value.
  static vectors::MetadataFilter OwnerFilter(const std::string& owner) {
    vectors::MetadataFilter filter;
    vectors::FilterCondition cond;
    cond.field = "owner";
    cond.op = vectors::FilterOp::kEq;
    cond.value = owner;
    filter.conditions.push_back(cond);
    return filter;
  }

  config::EventsConfig events_config_;
  config::VectorsConfig vectors_config_;
  config::SimilarityConfig similarity_config_;

  std::unique_ptr<events::EventStore> event_store_;
  std::unique_ptr<events::CoOccurrenceIndex> co_index_;
  std::unique_ptr<vectors::VectorStore> vector_store_;
  std::unique_ptr<vectors::MetadataStore> metadata_store_;
  std::unique_ptr<SimilarityEngine> engine_;
};

TEST_F(MetadataDesyncTest, SlotReuseDoesNotLeakDeletedMetadata) {
  // Insert several vectors. "alice_item" carries owner=alice metadata.
  ASSERT_TRUE(vector_store_->SetVector("alice_item", {1.0f, 0.0f, 0.0f}).has_value());
  ASSERT_TRUE(vector_store_->SetVector("bob_item", {0.0f, 1.0f, 0.0f}).has_value());
  ASSERT_TRUE(vector_store_->SetVector("carol_item", {0.0f, 0.0f, 1.0f}).has_value());

  metadata_store_->Set("alice_item", {{"owner", std::string("alice")}});
  metadata_store_->Set("bob_item", {{"owner", std::string("bob")}});

  // Sanity: querying owner=alice returns only alice_item.
  {
    auto results = engine_->SearchByVector({1.0f, 1.0f, 1.0f}, 10, OwnerFilter("alice"));
    ASSERT_TRUE(results.has_value());
    ASSERT_EQ(results->size(), 1U);
    EXPECT_EQ((*results)[0].item_id, "alice_item");
  }

  // Delete alice_item from the vector store and, as a delete handler must,
  // delete its metadata too. With auto-defrag (>25% tombstones) the underlying
  // slot index is recycled; a newly inserted vector reuses alice's old index.
  ASSERT_TRUE(vector_store_->DeleteVector("alice_item"));
  metadata_store_->Delete("alice_item");
  // Force a defragment to guarantee index churn regardless of the auto-defrag
  // threshold heuristic.
  vector_store_->Defragment();

  // Insert a new vector that has NO metadata. Under the old compact-index
  // keying it would inherit alice's metadata from the recycled slot.
  ASSERT_TRUE(vector_store_->SetVector("dave_item", {0.5f, 0.5f, 0.5f}).has_value());

  // The deleted item's metadata must be gone.
  EXPECT_EQ(metadata_store_->Get("alice_item"), nullptr);
  // The new item must not have inherited any metadata.
  EXPECT_EQ(metadata_store_->Get("dave_item"), nullptr);

  // A filter for owner=alice must now return nothing: alice_item is deleted and
  // no live item should match. This property holds purely from id-keying even
  // if metadata Delete were skipped, because the leaked entry is keyed by a
  // now-dead id that no live vector references.
  {
    auto results = engine_->SearchByVector({1.0f, 1.0f, 1.0f}, 10, OwnerFilter("alice"));
    ASSERT_TRUE(results.has_value());
    for (const auto& r : *results) {
      EXPECT_NE(r.item_id, "alice_item");
      EXPECT_NE(r.item_id, "dave_item");
    }
    EXPECT_TRUE(results->empty());
  }

  // bob_item's metadata must still be intact and correctly associated.
  {
    auto results = engine_->SearchByVector({1.0f, 1.0f, 1.0f}, 10, OwnerFilter("bob"));
    ASSERT_TRUE(results.has_value());
    ASSERT_EQ(results->size(), 1U);
    EXPECT_EQ((*results)[0].item_id, "bob_item");
  }
}

// ============================================================================
// Additive Fusion Union (H-4)
// ============================================================================

// Regression test for H-4: fusion must treat the vector and co-occurrence
// signals as additive (a UNION of candidates), not an intersection. A
// content-similar item that is NOT a co-occurrence neighbor of the query must
// still be able to surface via the vector signal.
TEST_F(SimilarityEngineTest, FusionSurfacesContentSimilarNonNeighbor) {
  // The query "q" co-occurs only with "co_a" and "co_b" (its event neighbors).
  std::vector<events::Event> events = {{"q", 10, 1000}, {"co_a", 20, 1001}, {"co_b", 15, 1002}};
  co_index_->UpdateFromEvents("ctx1", events);

  // Vectors: "content" is nearly identical to "q" but has NO co-occurrence with
  // it. The co-occurrence neighbors point in a different direction.
  ASSERT_TRUE(vector_store_->SetVector("q", {1.0f, 0.0f, 0.0f}).has_value());
  ASSERT_TRUE(vector_store_->SetVector("content", {0.99f, 0.01f, 0.0f}).has_value());
  ASSERT_TRUE(vector_store_->SetVector("co_a", {0.0f, 1.0f, 0.0f}).has_value());
  ASSERT_TRUE(vector_store_->SetVector("co_b", {0.0f, 0.9f, 0.1f}).has_value());

  // Sanity: "content" is NOT an event neighbor of "q".
  auto event_results = engine_->SearchByIdEvents("q", 10);
  ASSERT_TRUE(event_results.has_value());
  for (const auto& r : *event_results) {
    EXPECT_NE(r.item_id, "content") << "content must not be a co-occurrence neighbor";
  }

  // Fusion must surface "content" purely from the vector signal.
  auto results = engine_->SearchByIdFusion("q", 10);
  ASSERT_TRUE(results.has_value());
  bool found_content = false;
  for (const auto& r : *results) {
    if (r.item_id == "content") {
      found_content = true;
    }
  }
  EXPECT_TRUE(found_content) << "fusion must union the vector signal, surfacing a content-similar non-neighbor";
}

// ============================================================================
// Score Normalization (M-14)
// ============================================================================

// M-14: NormalizeScores must NOT collapse a single candidate to a fixed 0.5.
// A single strong match and a single weak match must end up with different
// (absolute-confidence-preserving) scores.
TEST_F(SimilarityEngineTest, NormalizeSingleResultPreservesAbsoluteConfidence) {
  // One strong vector neighbor (cosine ~1.0).
  ASSERT_TRUE(vector_store_->SetVector("query_strong", {1.0f, 0.0f, 0.0f}).has_value());
  ASSERT_TRUE(vector_store_->SetVector("near", {1.0f, 0.0f, 0.0f}).has_value());
  auto strong = engine_->SearchByIdVectors("query_strong", 1);
  ASSERT_TRUE(strong.has_value());
  ASSERT_EQ(strong->size(), 1U);
  // The raw cosine here is ~1.0; the single-result fusion path would later
  // normalize it. Verify the engine reports a high absolute score rather than
  // a flattened mid value.
  EXPECT_GT((*strong)[0].score, 0.9f);

  // One weak query: orthogonal to every vector already in the store, so its
  // best single neighbor has cosine ~0.0. (Using a {1,0,0} query here would
  // match the existing {1,0,0} vectors perfectly, defeating the check.)
  ASSERT_TRUE(vector_store_->SetVector("query_weak", {0.0f, 0.0f, 1.0f}).has_value());
  ASSERT_TRUE(vector_store_->SetVector("far", {0.0f, 1.0f, 0.0f}).has_value());
  auto weak = engine_->SearchByIdVectors("query_weak", 1);
  ASSERT_TRUE(weak.has_value());

  // A strong single match (cosine ~1.0) and a weak single match (cosine ~0.0)
  // must report their true absolute confidence, not both collapse to the same
  // value the way a fixed 0.5 or min-max single-result fallback would.
  if (!weak->empty()) {
    EXPECT_LT((*weak)[0].score, (*strong)[0].score);
  }
}

// M-14: when all candidate scores are equal, fusion must preserve the shared
// absolute confidence (clamped) instead of forcing every score to 0.5. A
// cluster of strong matches keeps high fusion weight.
TEST_F(SimilarityEngineTest, FusionAllEqualScoresStayHigh) {
  // All vectors identical -> all cosine similarities are ~1.0 (degenerate range).
  ASSERT_TRUE(vector_store_->SetVector("q", {1.0f, 0.0f, 0.0f}).has_value());
  ASSERT_TRUE(vector_store_->SetVector("a", {1.0f, 0.0f, 0.0f}).has_value());
  ASSERT_TRUE(vector_store_->SetVector("b", {1.0f, 0.0f, 0.0f}).has_value());

  // No co-occurrence: fusion uses only the (all-equal) vector signal.
  // alpha defaults weight the vector source; with preserved absolute confidence
  // (clamped ~1.0) the fused scores must stay clearly above the old 0.5 floor.
  auto results = engine_->SearchByIdFusion("q", 10, /*adaptive=*/false);
  ASSERT_TRUE(results.has_value());
  ASSERT_FALSE(results->empty());
  for (const auto& r : *results) {
    // fusion_alpha (0.6) * clamped(~1.0) == ~0.6 > the 0.5 collapse value.
    EXPECT_GT(r.score, 0.5f) << "all-equal high-similarity cluster must retain confidence";
  }
}

// ============================================================================
// Deterministic Tie-Break (#19)
// ============================================================================

// #19: identical scores must produce a deterministic, id-ordered result that is
// stable across repeated runs (no reliance on insertion or hash order).
TEST_F(SimilarityEngineTest, IdenticalScoresAreDeterministicallyOrdered) {
  // All vectors identical -> identical cosine scores for every candidate.
  ASSERT_TRUE(vector_store_->SetVector("query", {1.0f, 0.0f, 0.0f}).has_value());
  ASSERT_TRUE(vector_store_->SetVector("d", {1.0f, 0.0f, 0.0f}).has_value());
  ASSERT_TRUE(vector_store_->SetVector("a", {1.0f, 0.0f, 0.0f}).has_value());
  ASSERT_TRUE(vector_store_->SetVector("c", {1.0f, 0.0f, 0.0f}).has_value());
  ASSERT_TRUE(vector_store_->SetVector("b", {1.0f, 0.0f, 0.0f}).has_value());

  std::vector<std::string> first_order;
  {
    auto results = engine_->SearchByIdVectors("query", 10);
    ASSERT_TRUE(results.has_value());
    for (const auto& r : *results) {
      first_order.push_back(r.item_id);
    }
  }
  ASSERT_FALSE(first_order.empty());

  // The tie-break is item_id ascending, so the order must be sorted by id.
  EXPECT_TRUE(std::is_sorted(first_order.begin(), first_order.end()))
      << "tied scores must be ordered by ascending item_id";

  // Repeated runs must yield the identical ordering.
  for (int run = 0; run < 5; ++run) {
    auto results = engine_->SearchByIdVectors("query", 10);
    ASSERT_TRUE(results.has_value());
    std::vector<std::string> order;
    for (const auto& r : *results) {
      order.push_back(r.item_id);
    }
    EXPECT_EQ(order, first_order) << "ordering must be deterministic across runs";
  }
}

}  // namespace
}  // namespace nvecd::similarity
