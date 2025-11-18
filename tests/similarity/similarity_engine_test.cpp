/**
 * @file similarity_engine_test.cpp
 * @brief Unit tests for SimilarityEngine
 */

#include "similarity/similarity_engine.h"

#include <gtest/gtest.h>

#include <unordered_set>

#include "events/co_occurrence_index.h"
#include "events/event_store.h"
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

    event_store_ =
        std::make_unique<events::EventStore>(events_config_);
    co_index_ = std::make_unique<events::CoOccurrenceIndex>();
    vector_store_ =
        std::make_unique<vectors::VectorStore>(vectors_config_);

    engine_ = std::make_unique<SimilarityEngine>(
        event_store_.get(), co_index_.get(), vector_store_.get(),
        similarity_config_);
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
  std::vector<events::Event> events = {
      {"item1", 10, 1000}, {"item2", 20, 1001}, {"item3", 15, 1002}};

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

  auto result2 = engine_->SearchByIdEvents("item1", -1);
  EXPECT_FALSE(result2.has_value());

  auto result3 = engine_->SearchByIdEvents("item1", 10000);
  EXPECT_FALSE(result3.has_value());
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
    EXPECT_EQ((*results)[0].id, "item2");
  }
}

TEST_F(SimilarityEngineTest, SearchByIdVectors_SortedByScore) {
  // Add vectors with known similarities
  ASSERT_TRUE(vector_store_->SetVector("item1", {1.0f, 0.0f, 0.0f}).has_value());
  ASSERT_TRUE(vector_store_->SetVector("item2", {0.9f, 0.1f, 0.0f}).has_value());  // Very similar
  ASSERT_TRUE(vector_store_->SetVector("item3", {0.0f, 1.0f, 0.0f}).has_value());  // Orthogonal
  ASSERT_TRUE(vector_store_->SetVector("item4", {-1.0f, 0.0f, 0.0f}).has_value()); // Opposite

  auto results = engine_->SearchByIdVectors("item1", 10);
  ASSERT_TRUE(results.has_value());
  ASSERT_GE(results->size(), 3);

  // Results should be sorted by score descending
  for (size_t i = 1; i < results->size(); ++i) {
    EXPECT_GE((*results)[i - 1].score, (*results)[i].score);
  }

  // item2 should be most similar
  EXPECT_EQ((*results)[0].id, "item2");
}

// ============================================================================
// Fusion Search Tests
// ============================================================================

TEST_F(SimilarityEngineTest, SearchByIdFusion_BothEmpty) {
  auto results = engine_->SearchByIdFusion("item1", 10);
  ASSERT_TRUE(results.has_value());
  EXPECT_TRUE(results->empty());  // No results from either source
}

TEST_F(SimilarityEngineTest, SearchByIdFusion_OnlyEvents) {
  // Add events
  std::vector<events::Event> events = {
      {"item1", 10, 1000}, {"item2", 20, 1001}, {"item3", 15, 1002}};
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
  std::vector<events::Event> events = {
      {"item1", 10, 1000}, {"item2", 20, 1001}, {"item3", 15, 1002}};
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
    ASSERT_TRUE(
        vector_store_->SetVector("item" + std::to_string(i), {val, val, val})
            .has_value());
  }

  std::vector<float> query = {0.5f, 0.5f, 0.5f};
  auto results = engine_->SearchByVector(query, 5);
  ASSERT_TRUE(results.has_value());
  EXPECT_LE(results->size(), 5);
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
    EXPECT_GE((*results)[i - 1].score, (*results)[i].score)
        << "Results not in descending order at index " << i;
  }
}

TEST_F(SimilarityEngineTest, NoDuplicatesInFusion) {
  // Add events and vectors for same items
  std::vector<events::Event> events = {
      {"item1", 10, 1000}, {"item2", 20, 1001}, {"item3", 15, 1002}};
  co_index_->UpdateFromEvents("ctx1", events);

  ASSERT_TRUE(vector_store_->SetVector("item1", {0.1f, 0.2f, 0.3f}).has_value());
  ASSERT_TRUE(vector_store_->SetVector("item2", {0.15f, 0.25f, 0.35f}).has_value());
  ASSERT_TRUE(vector_store_->SetVector("item3", {0.9f, 0.8f, 0.7f}).has_value());

  auto results = engine_->SearchByIdFusion("item1", 10);
  ASSERT_TRUE(results.has_value());

  // Check for duplicates
  std::unordered_set<std::string> seen_ids;
  for (const auto& result : *results) {
    EXPECT_EQ(seen_ids.count(result.id), 0) << "Duplicate ID: " << result.id;
    seen_ids.insert(result.id);
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
  std::vector<events::Event> events = {
      {"item1", 100, 1000}, {"item2", 50, 1001}, {"item3", 25, 1002}};
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
    EXPECT_EQ((*results)[0].id, (*vector_results)[0].id);
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
  std::vector<events::Event> events = {
      {"item1", 100, 1000}, {"item2", 50, 1001}, {"item3", 25, 1002}};
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
    EXPECT_EQ((*results)[0].id, (*event_results)[0].id);
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
  ASSERT_TRUE(vector_store_->SetVector("item_high_event", {0.9f, 0.8f, 0.7f}).has_value());  // Different vector
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
    if ((*results)[i].id == "item_high_event") {
      high_event_found = true;
      high_event_pos = i;
    }
    if ((*results)[i].id == "item_high_vector") {
      high_vector_found = true;
      high_vector_pos = i;
    }
  }

  if (high_event_found && high_vector_found) {
    EXPECT_LT(high_vector_pos, high_event_pos)
        << "With alpha=0.8, vector similarity should dominate";
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
  ASSERT_TRUE(vector_store_->SetVector("item_high_event", {0.9f, 0.8f, 0.7f}).has_value());  // Different vector
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
    if ((*results)[i].id == "item_high_event") {
      high_event_found = true;
      high_event_pos = i;
    }
    if ((*results)[i].id == "item_high_vector") {
      high_vector_found = true;
      high_vector_pos = i;
    }
  }

  if (high_event_found && high_vector_found) {
    EXPECT_LT(high_event_pos, high_vector_pos)
        << "With beta=0.8, event score should dominate";
  }
}

}  // namespace
}  // namespace nvecd::similarity
