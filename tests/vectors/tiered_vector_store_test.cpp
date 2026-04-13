/**
 * @file tiered_vector_store_test.cpp
 * @brief Tests for TieredVectorStore and MergeScheduler
 */

#include "vectors/tiered_vector_store.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <random>
#include <thread>
#include <vector>

#include "vectors/merge_scheduler.h"

namespace nvecd::vectors {
namespace {

// ============================================================================
// Helpers
// ============================================================================

/// Create a simple unit vector along the given axis (1-hot)
std::vector<float> MakeAxisVector(uint32_t dim, uint32_t axis) {
  std::vector<float> v(dim, 0.0F);
  if (axis < dim) {
    v[axis] = 1.0F;
  }
  return v;
}

/// Create a random normalized vector
std::vector<float> MakeRandomVector(uint32_t dim, std::mt19937& rng) {
  std::normal_distribution<float> dist(0.0F, 1.0F);
  std::vector<float> v(dim);
  float norm = 0.0F;
  for (auto& x : v) {
    x = dist(rng);
    norm += x * x;
  }
  norm = std::sqrt(norm);
  for (auto& x : v) {
    x /= norm;
  }
  return v;
}

TieredVectorStore::Config DefaultConfig() {
  TieredVectorStore::Config cfg;
  cfg.delta_merge_threshold = 100;
  cfg.tombstone_ratio_threshold = 0.1F;
  cfg.distance_metric = "cosine";
  cfg.hnsw_m = 8;
  cfg.hnsw_ef_construction = 50;
  cfg.hnsw_ef_search = 30;
  return cfg;
}

// ============================================================================
// Basic Add / Delete / Update
// ============================================================================

class TieredVectorStoreTest : public ::testing::Test {
 protected:
  static constexpr uint32_t kDim = 8;

  void SetUp() override {
    store_ = std::make_unique<TieredVectorStore>(DefaultConfig());
  }

  std::unique_ptr<TieredVectorStore> store_;
};

TEST_F(TieredVectorStoreTest, AddSingleVector) {
  auto result = store_->Add("v1", MakeAxisVector(kDim, 0));
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(store_->TotalSize(), 1U);
  EXPECT_TRUE(store_->HasVector("v1"));
  EXPECT_TRUE(store_->IsInDelta("v1"));
  EXPECT_EQ(store_->GetDimension(), kDim);
}

TEST_F(TieredVectorStoreTest, AddEmptyVectorFails) {
  auto result = store_->Add("v1", {});
  EXPECT_FALSE(result.has_value());
}

TEST_F(TieredVectorStoreTest, AddDimensionMismatchFails) {
  store_->Add("v1", MakeAxisVector(kDim, 0));
  auto result = store_->Add("v2", MakeAxisVector(kDim + 1, 0));
  EXPECT_FALSE(result.has_value());
  EXPECT_EQ(store_->TotalSize(), 1U);
}

TEST_F(TieredVectorStoreTest, AddDuplicateIdReplacesInDelta) {
  store_->Add("v1", MakeAxisVector(kDim, 0));
  store_->Add("v1", MakeAxisVector(kDim, 1));
  EXPECT_EQ(store_->TotalSize(), 1U);
  EXPECT_TRUE(store_->IsInDelta("v1"));
}

TEST_F(TieredVectorStoreTest, DeleteFromDelta) {
  store_->Add("v1", MakeAxisVector(kDim, 0));
  auto result = store_->Delete("v1");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(store_->TotalSize(), 0U);
  EXPECT_FALSE(store_->HasVector("v1"));
}

TEST_F(TieredVectorStoreTest, DeleteNonExistentFails) {
  auto result = store_->Delete("nonexistent");
  EXPECT_FALSE(result.has_value());
}

TEST_F(TieredVectorStoreTest, UpdateVector) {
  store_->Add("v1", MakeAxisVector(kDim, 0));
  auto result = store_->Update("v1", MakeAxisVector(kDim, 1));
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(store_->TotalSize(), 1U);
}

TEST_F(TieredVectorStoreTest, UpdateNonExistentCreatesNew) {
  auto result = store_->Update("v1", MakeAxisVector(kDim, 0));
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(store_->TotalSize(), 1U);
}

// ============================================================================
// Search
// ============================================================================

TEST_F(TieredVectorStoreTest, SearchEmptyStore) {
  auto query = MakeAxisVector(kDim, 0);
  auto results = store_->Search(query.data(), 5);
  EXPECT_TRUE(results.empty());
}

TEST_F(TieredVectorStoreTest, SearchDeltaOnly) {
  store_->Add("v0", MakeAxisVector(kDim, 0));
  store_->Add("v1", MakeAxisVector(kDim, 1));
  store_->Add("v2", MakeAxisVector(kDim, 2));

  auto query = MakeAxisVector(kDim, 0);
  auto results = store_->Search(query.data(), 2);

  ASSERT_GE(results.size(), 1U);
  // Exact match should be first
  EXPECT_EQ(results[0].id, "v0");
  EXPECT_NEAR(results[0].score, 1.0F, 0.01F);
}

TEST_F(TieredVectorStoreTest, SearchMainOnly) {
  store_->Add("v0", MakeAxisVector(kDim, 0));
  store_->Add("v1", MakeAxisVector(kDim, 1));
  store_->MergeDeltaToMain();

  EXPECT_EQ(store_->DeltaSize(), 0U);
  EXPECT_EQ(store_->MainSize(), 2U);

  auto query = MakeAxisVector(kDim, 0);
  auto results = store_->Search(query.data(), 2);

  ASSERT_GE(results.size(), 1U);
  EXPECT_EQ(results[0].id, "v0");
  EXPECT_NEAR(results[0].score, 1.0F, 0.01F);
}

TEST_F(TieredVectorStoreTest, SearchMergesMainAndDelta) {
  // Add vectors to main
  store_->Add("v0", MakeAxisVector(kDim, 0));
  store_->Add("v1", MakeAxisVector(kDim, 1));
  store_->MergeDeltaToMain();

  // Add more to delta
  store_->Add("v2", MakeAxisVector(kDim, 2));

  auto query = MakeAxisVector(kDim, 2);
  auto results = store_->Search(query.data(), 3);

  ASSERT_GE(results.size(), 1U);
  EXPECT_EQ(results[0].id, "v2");
  EXPECT_NEAR(results[0].score, 1.0F, 0.01F);
}

TEST_F(TieredVectorStoreTest, SearchExcludesDeletedFromMain) {
  store_->Add("v0", MakeAxisVector(kDim, 0));
  store_->Add("v1", MakeAxisVector(kDim, 1));
  store_->MergeDeltaToMain();

  store_->Delete("v0");
  EXPECT_EQ(store_->DeletedCount(), 1U);

  auto query = MakeAxisVector(kDim, 0);
  auto results = store_->Search(query.data(), 5);

  // v0 should not appear in results
  for (const auto& r : results) {
    EXPECT_NE(r.id, "v0");
  }
}

TEST_F(TieredVectorStoreTest, SearchTopKLimitsResults) {
  for (uint32_t i = 0; i < kDim; ++i) {
    store_->Add("v" + std::to_string(i), MakeAxisVector(kDim, i));
  }

  auto query = MakeAxisVector(kDim, 0);
  auto results = store_->Search(query.data(), 3);
  EXPECT_LE(results.size(), 3U);
}

TEST_F(TieredVectorStoreTest, SearchAfterUpdate) {
  // v1 points along axis 0
  store_->Add("v1", MakeAxisVector(kDim, 0));
  store_->MergeDeltaToMain();

  // Update v1 to point along axis 1
  store_->Update("v1", MakeAxisVector(kDim, 1));

  // Search for axis 1 — should find updated v1
  auto query = MakeAxisVector(kDim, 1);
  auto results = store_->Search(query.data(), 1);
  ASSERT_EQ(results.size(), 1U);
  EXPECT_EQ(results[0].id, "v1");
  EXPECT_NEAR(results[0].score, 1.0F, 0.01F);

  // Search for axis 0 — v1 is the only vector, so it will be returned
  // but its score should be ~0 (orthogonal to query)
  auto query0 = MakeAxisVector(kDim, 0);
  auto results0 = store_->Search(query0.data(), 1);
  if (!results0.empty()) {
    EXPECT_NEAR(results0[0].score, 0.0F, 0.01F);
  }
}

// ============================================================================
// Merge / Rebuild
// ============================================================================

TEST_F(TieredVectorStoreTest, MergeDeltaToMain) {
  store_->Add("v0", MakeAxisVector(kDim, 0));
  store_->Add("v1", MakeAxisVector(kDim, 1));
  EXPECT_EQ(store_->DeltaSize(), 2U);
  EXPECT_EQ(store_->MainSize(), 0U);

  auto result = store_->MergeDeltaToMain();
  ASSERT_TRUE(result.has_value());

  EXPECT_EQ(store_->DeltaSize(), 0U);
  EXPECT_EQ(store_->MainSize(), 2U);
  EXPECT_TRUE(store_->IsInMain("v0"));
  EXPECT_TRUE(store_->IsInMain("v1"));
}

TEST_F(TieredVectorStoreTest, MergeEmptyDeltaIsNoop) {
  auto result = store_->MergeDeltaToMain();
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(store_->MainSize(), 0U);
}

TEST_F(TieredVectorStoreTest, RebuildMainCompactsDeleted) {
  store_->Add("v0", MakeAxisVector(kDim, 0));
  store_->Add("v1", MakeAxisVector(kDim, 1));
  store_->Add("v2", MakeAxisVector(kDim, 2));
  store_->MergeDeltaToMain();

  store_->Delete("v1");
  EXPECT_EQ(store_->DeletedCount(), 1U);
  EXPECT_EQ(store_->MainSize(), 2U);

  auto result = store_->RebuildMain();
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(store_->DeletedCount(), 0U);
  EXPECT_EQ(store_->MainSize(), 2U);
  EXPECT_FALSE(store_->HasVector("v1"));
}

TEST_F(TieredVectorStoreTest, RebuildNoDeletedIsNoop) {
  store_->Add("v0", MakeAxisVector(kDim, 0));
  store_->MergeDeltaToMain();

  auto result = store_->RebuildMain();
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(store_->MainSize(), 1U);
}

TEST_F(TieredVectorStoreTest, SearchAfterRebuild) {
  store_->Add("v0", MakeAxisVector(kDim, 0));
  store_->Add("v1", MakeAxisVector(kDim, 1));
  store_->Add("v2", MakeAxisVector(kDim, 2));
  store_->MergeDeltaToMain();

  store_->Delete("v1");
  store_->RebuildMain();

  auto query = MakeAxisVector(kDim, 0);
  auto results = store_->Search(query.data(), 3);
  ASSERT_GE(results.size(), 1U);
  EXPECT_EQ(results[0].id, "v0");
  for (const auto& r : results) {
    EXPECT_NE(r.id, "v1");
  }
}

// ============================================================================
// NeedsMerge / NeedsRebuild thresholds
// ============================================================================

TEST_F(TieredVectorStoreTest, NeedsMergeThreshold) {
  auto cfg = DefaultConfig();
  cfg.delta_merge_threshold = 5;
  store_ = std::make_unique<TieredVectorStore>(cfg);

  for (uint32_t i = 0; i < 4; ++i) {
    store_->Add("v" + std::to_string(i), MakeAxisVector(kDim, i % kDim));
  }
  EXPECT_FALSE(store_->NeedsMerge());

  store_->Add("v4", MakeAxisVector(kDim, 4 % kDim));
  EXPECT_TRUE(store_->NeedsMerge());
}

TEST_F(TieredVectorStoreTest, NeedsRebuildThreshold) {
  auto cfg = DefaultConfig();
  cfg.tombstone_ratio_threshold = 0.2F;
  store_ = std::make_unique<TieredVectorStore>(cfg);

  // Add 5 vectors to main
  for (uint32_t i = 0; i < 5; ++i) {
    store_->Add("v" + std::to_string(i), MakeAxisVector(kDim, i % kDim));
  }
  store_->MergeDeltaToMain();
  EXPECT_FALSE(store_->NeedsRebuild());

  // Delete 1 of 5 = 20% = threshold, need > threshold
  store_->Delete("v0");
  EXPECT_FALSE(store_->NeedsRebuild());  // 20% == threshold, not exceeded

  // Delete 2 of 5 = 40% > 20%
  store_->Delete("v1");
  EXPECT_TRUE(store_->NeedsRebuild());
}

// ============================================================================
// Swap-delete correctness in delta
// ============================================================================

TEST_F(TieredVectorStoreTest, DeleteMiddleOfDeltaPreservesOthers) {
  store_->Add("v0", MakeAxisVector(kDim, 0));
  store_->Add("v1", MakeAxisVector(kDim, 1));
  store_->Add("v2", MakeAxisVector(kDim, 2));

  // Delete middle element
  store_->Delete("v1");
  EXPECT_EQ(store_->DeltaSize(), 2U);
  EXPECT_TRUE(store_->HasVector("v0"));
  EXPECT_FALSE(store_->HasVector("v1"));
  EXPECT_TRUE(store_->HasVector("v2"));

  // Search should still find v0 and v2
  auto query = MakeAxisVector(kDim, 0);
  auto results = store_->Search(query.data(), 5);
  ASSERT_GE(results.size(), 1U);
  EXPECT_EQ(results[0].id, "v0");
}

TEST_F(TieredVectorStoreTest, DeleteAllFromDelta) {
  store_->Add("v0", MakeAxisVector(kDim, 0));
  store_->Add("v1", MakeAxisVector(kDim, 1));
  store_->Delete("v0");
  store_->Delete("v1");
  EXPECT_EQ(store_->TotalSize(), 0U);
}

// ============================================================================
// Cross-tier operations
// ============================================================================

TEST_F(TieredVectorStoreTest, AddDuplicateMovesFromMainToDelta) {
  store_->Add("v1", MakeAxisVector(kDim, 0));
  store_->MergeDeltaToMain();
  EXPECT_TRUE(store_->IsInMain("v1"));

  // Re-add with different vector: should delete from main, add to delta
  store_->Add("v1", MakeAxisVector(kDim, 1));
  EXPECT_TRUE(store_->IsInDelta("v1"));
  EXPECT_EQ(store_->TotalSize(), 1U);
  EXPECT_EQ(store_->DeletedCount(), 1U);  // old main entry is tombstoned
}

TEST_F(TieredVectorStoreTest, DeleteFromMainCreatesTombstone) {
  store_->Add("v0", MakeAxisVector(kDim, 0));
  store_->MergeDeltaToMain();
  store_->Delete("v0");
  EXPECT_EQ(store_->MainSize(), 0U);
  EXPECT_EQ(store_->DeletedCount(), 1U);
}

// ============================================================================
// Multiple merges
// ============================================================================

TEST_F(TieredVectorStoreTest, MultipleMergesAccumulate) {
  store_->Add("v0", MakeAxisVector(kDim, 0));
  store_->MergeDeltaToMain();

  store_->Add("v1", MakeAxisVector(kDim, 1));
  store_->MergeDeltaToMain();

  store_->Add("v2", MakeAxisVector(kDim, 2));
  store_->MergeDeltaToMain();

  EXPECT_EQ(store_->MainSize(), 3U);
  EXPECT_EQ(store_->DeltaSize(), 0U);
  EXPECT_TRUE(store_->IsInMain("v0"));
  EXPECT_TRUE(store_->IsInMain("v1"));
  EXPECT_TRUE(store_->IsInMain("v2"));

  // All should be searchable
  auto query = MakeAxisVector(kDim, 1);
  auto results = store_->Search(query.data(), 3);
  ASSERT_GE(results.size(), 1U);
  EXPECT_EQ(results[0].id, "v1");
}

// ============================================================================
// Recall test with random vectors
// ============================================================================

TEST_F(TieredVectorStoreTest, RecallWithRandomVectors) {
  constexpr uint32_t kCount = 200;
  constexpr uint32_t kTopK = 10;
  std::mt19937 rng(42);

  auto cfg = DefaultConfig();
  cfg.delta_merge_threshold = 50;
  store_ = std::make_unique<TieredVectorStore>(cfg);

  // Add vectors: first 100 go to main, next 100 stay in delta
  for (uint32_t i = 0; i < kCount; ++i) {
    store_->Add("v" + std::to_string(i), MakeRandomVector(kDim, rng));
  }
  // Merge first batch
  // (all 200 are in delta; merge moves them to main)
  store_->MergeDeltaToMain();

  // Add 50 more to delta
  for (uint32_t i = kCount; i < kCount + 50; ++i) {
    store_->Add("v" + std::to_string(i), MakeRandomVector(kDim, rng));
  }

  EXPECT_EQ(store_->MainSize(), kCount);
  EXPECT_EQ(store_->DeltaSize(), 50U);

  // Search should return kTopK results from merged tiers
  auto query = MakeRandomVector(kDim, rng);
  auto results = store_->Search(query.data(), kTopK);
  EXPECT_EQ(results.size(), kTopK);

  // Verify descending score order
  for (size_t i = 1; i < results.size(); ++i) {
    EXPECT_GE(results[i - 1].score, results[i].score);
  }
}

// ============================================================================
// Concurrent reads
// ============================================================================

TEST_F(TieredVectorStoreTest, ConcurrentSearches) {
  constexpr uint32_t kCount = 50;
  std::mt19937 rng(123);

  for (uint32_t i = 0; i < kCount; ++i) {
    store_->Add("v" + std::to_string(i), MakeRandomVector(kDim, rng));
  }
  store_->MergeDeltaToMain();

  // Add some to delta too
  for (uint32_t i = kCount; i < kCount + 10; ++i) {
    store_->Add("v" + std::to_string(i), MakeRandomVector(kDim, rng));
  }

  constexpr int kThreads = 4;
  constexpr int kSearchesPerThread = 100;
  std::vector<std::thread> threads;
  std::atomic<int> total_results{0};

  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&, t] {
      std::mt19937 local_rng(t * 1000);
      for (int s = 0; s < kSearchesPerThread; ++s) {
        auto q = MakeRandomVector(kDim, local_rng);
        auto results = store_->Search(q.data(), 5);
        total_results.fetch_add(static_cast<int>(results.size()));
      }
    });
  }

  for (auto& th : threads) {
    th.join();
  }

  EXPECT_GT(total_results.load(), 0);
}

// ============================================================================
// MergeScheduler
// ============================================================================

TEST(MergeSchedulerTest, StartAndStop) {
  TieredVectorStore::Config cfg = DefaultConfig();
  TieredVectorStore store(cfg);

  MergeScheduler::Config sched_cfg;
  sched_cfg.check_interval = std::chrono::seconds(1);
  MergeScheduler scheduler(sched_cfg);

  scheduler.Start(&store);
  EXPECT_TRUE(scheduler.IsRunning());

  scheduler.Stop();
  EXPECT_FALSE(scheduler.IsRunning());
}

TEST(MergeSchedulerTest, AutoMergeOnThreshold) {
  constexpr uint32_t kDim = 4;

  TieredVectorStore::Config cfg = DefaultConfig();
  cfg.delta_merge_threshold = 5;
  TieredVectorStore store(cfg);

  // Add vectors exceeding threshold
  for (uint32_t i = 0; i < 6; ++i) {
    store.Add("v" + std::to_string(i), MakeAxisVector(kDim, i % kDim));
  }
  EXPECT_TRUE(store.NeedsMerge());

  MergeScheduler::Config sched_cfg;
  sched_cfg.check_interval = std::chrono::seconds(1);
  MergeScheduler scheduler(sched_cfg);
  scheduler.Start(&store);

  // Wait for scheduler to run at least once
  std::this_thread::sleep_for(std::chrono::seconds(2));

  scheduler.Stop();

  // After scheduler ran, delta should have been merged
  EXPECT_EQ(store.DeltaSize(), 0U);
  EXPECT_EQ(store.MainSize(), 6U);
}

TEST(MergeSchedulerTest, DoubleStartIsHarmless) {
  TieredVectorStore::Config cfg = DefaultConfig();
  TieredVectorStore store(cfg);

  MergeScheduler scheduler;
  scheduler.Start(&store);
  scheduler.Start(&store);  // Should not crash or create extra threads
  EXPECT_TRUE(scheduler.IsRunning());
  scheduler.Stop();
}

TEST(MergeSchedulerTest, DestructorStopsThread) {
  TieredVectorStore::Config cfg = DefaultConfig();
  TieredVectorStore store(cfg);

  {
    MergeScheduler scheduler;
    scheduler.Start(&store);
    EXPECT_TRUE(scheduler.IsRunning());
  }
  // Destructor should have joined the thread without crash
}

}  // namespace
}  // namespace nvecd::vectors
