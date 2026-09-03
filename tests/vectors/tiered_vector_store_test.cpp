/**
 * @file tiered_vector_store_test.cpp
 * @brief Tests for TieredVectorStore and MergeScheduler
 */

#include "vectors/tiered_vector_store.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <random>
#include <thread>
#include <unordered_set>
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

  void SetUp() override { store_ = std::make_unique<TieredVectorStore>(DefaultConfig()); }

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

// Regression test for H-3: after overwriting/deleting several vectors in MAIN,
// the ANN post-filter must not consume its candidate budget on tombstones and
// return fewer than top_k results when enough live vectors still exist.
TEST_F(TieredVectorStoreTest, SearchReturnsTopKLiveResultsAfterMainDeletions) {
  constexpr uint32_t kBigDim = 32;
  constexpr uint32_t kCount = 200;
  TieredVectorStore::Config cfg = DefaultConfig();
  cfg.distance_metric = "cosine";
  TieredVectorStore store(cfg);

  std::mt19937 rng(12345);
  std::vector<std::vector<float>> vecs;
  vecs.reserve(kCount);
  for (uint32_t i = 0; i < kCount; ++i) {
    auto v = MakeRandomVector(kBigDim, rng);
    vecs.push_back(v);
    ASSERT_TRUE(store.Add("v" + std::to_string(i), v).has_value());
  }

  // Move everything into MAIN so the HNSW index drives the search.
  ASSERT_TRUE(store.MergeDeltaToMain().has_value());
  EXPECT_EQ(store.MainSize(), kCount);

  // Overwrite (delete-in-main + re-add-to-delta) the first 100 ids. This leaves
  // 100 tombstones in MAIN while their live copies live in DELTA, plus 100
  // untouched live MAIN vectors. Plenty of live vectors remain for top_k=20.
  for (uint32_t i = 0; i < 100; ++i) {
    ASSERT_TRUE(store.Add("v" + std::to_string(i), vecs[i]).has_value());
  }
  EXPECT_EQ(store.DeletedCount(), 100U);
  EXPECT_EQ(store.TotalSize(), kCount);  // No net change in live count.

  constexpr uint32_t kTopK = 20;
  auto query = MakeRandomVector(kBigDim, rng);
  auto results = store.Search(query.data(), kTopK);

  // Enough live vectors exist (200), so we must get a full top_k back, and no
  // tombstoned MAIN row may appear.
  EXPECT_EQ(results.size(), kTopK);
  std::unordered_set<std::string> seen;
  for (const auto& r : results) {
    EXPECT_TRUE(seen.insert(r.id).second) << "duplicate id: " << r.id;
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

// Replacing a main-tier id retires the same slot Delete retires, so the ANN
// index stops treating it as live rather than spending search budget on a row
// the tombstone filter then discards.
TEST_F(TieredVectorStoreTest, ReplacingMainEntryRetiresItFromSearch) {
  constexpr uint32_t kCount = 8;
  for (uint32_t i = 0; i < kCount; ++i) {
    store_->Add("v" + std::to_string(i), MakeAxisVector(kDim, i));
  }
  store_->MergeDeltaToMain();
  ASSERT_TRUE(store_->IsInMain("v0"));

  // Re-add v0 pointing along a different axis; the old main row must not be
  // reachable through search any more.
  store_->Add("v0", MakeAxisVector(kDim, kCount));

  auto query = MakeAxisVector(kDim, 0);
  auto results = store_->Search(query.data(), kCount);
  size_t v0_hits = 0;
  for (const auto& result : results) {
    if (result.id == "v0") {
      ++v0_hits;
      EXPECT_LT(result.score, 0.5F) << "stale embedding still ranked for v0";
    }
  }
  EXPECT_LE(v0_hits, 1U);
  EXPECT_EQ(store_->TotalSize(), kCount);
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

TEST_F(TieredVectorStoreTest, ConcurrentAddDeleteSearch) {
  constexpr int kWriters = 2;
  constexpr int kReaders = 4;
  constexpr int kOpsPerWriter = 50;
  constexpr int kSearchesPerReader = 50;
  std::atomic<bool> stop{false};
  std::atomic<int> search_results{0};

  // Pre-populate
  std::mt19937 rng(42);
  for (int i = 0; i < 20; ++i) {
    store_->Add("pre_" + std::to_string(i), MakeRandomVector(kDim, rng));
  }

  // Writer threads: add and delete
  std::vector<std::thread> threads;
  for (int w = 0; w < kWriters; ++w) {
    threads.emplace_back([&, w] {
      std::mt19937 local_rng(w * 1000);
      for (int i = 0; i < kOpsPerWriter; ++i) {
        std::string id = "w" + std::to_string(w) + "_" + std::to_string(i);
        store_->Add(id, MakeRandomVector(kDim, local_rng));
        if (i % 3 == 0) {
          store_->Delete(id);
        }
      }
    });
  }

  // Reader threads: search concurrently
  for (int r = 0; r < kReaders; ++r) {
    threads.emplace_back([&, r] {
      std::mt19937 local_rng(r * 2000);
      for (int i = 0; i < kSearchesPerReader && !stop.load(); ++i) {
        auto q = MakeRandomVector(kDim, local_rng);
        auto results = store_->Search(q.data(), 5);
        search_results.fetch_add(static_cast<int>(results.size()));
      }
    });
  }

  for (auto& th : threads) {
    th.join();
  }

  EXPECT_GT(search_results.load(), 0);
}

TEST_F(TieredVectorStoreTest, UpdateSameIdRapidly) {
  constexpr int kThreads = 4;
  constexpr int kUpdatesPerThread = 50;

  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&, t] {
      std::mt19937 local_rng(t * 100);
      for (int i = 0; i < kUpdatesPerThread; ++i) {
        store_->Update("shared_id", MakeRandomVector(kDim, local_rng));
      }
    });
  }

  for (auto& th : threads) {
    th.join();
  }

  // The vector should exist and be searchable
  auto query = MakeAxisVector(kDim, 0);
  auto results = store_->Search(query.data(), 5);
  // At least our shared_id should be findable
  bool found = false;
  for (const auto& [id, score] : results) {
    if (id == "shared_id")
      found = true;
  }
  EXPECT_TRUE(found);
}

// ============================================================================
// MergeScheduler
// ============================================================================

/// Poll until the predicate holds; report failure instead of blocking forever.
template <typename Predicate>
bool WaitFor(Predicate predicate, std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return predicate();
}

/// Shortest interval the scheduler config can express, to keep waits bounded.
MergeScheduler::Config FastSchedulerConfig() {
  MergeScheduler::Config config;
  config.check_interval = std::chrono::seconds(1);
  return config;
}

TEST(MergeSchedulerTest, StartAndStop) {
  TieredVectorStore::Config cfg = DefaultConfig();
  TieredVectorStore store(cfg);

  MergeScheduler scheduler(FastSchedulerConfig());

  scheduler.Start(&store);
  EXPECT_TRUE(scheduler.IsRunning());

  scheduler.Stop();
  EXPECT_FALSE(scheduler.IsRunning());
}

TEST(MergeSchedulerTest, AutoMergeOnThreshold) {
  constexpr uint32_t kDim = 4;
  constexpr uint32_t kVectorCount = 6;

  TieredVectorStore::Config cfg = DefaultConfig();
  cfg.delta_merge_threshold = 5;
  TieredVectorStore store(cfg);

  for (uint32_t i = 0; i < kVectorCount; ++i) {
    store.Add("v" + std::to_string(i), MakeAxisVector(kDim, i % kDim));
  }
  ASSERT_TRUE(store.NeedsMerge());
  ASSERT_EQ(store.DeltaSize(), kVectorCount);

  MergeScheduler scheduler(FastSchedulerConfig());
  scheduler.Start(&store);

  // Wait on the state transition itself rather than on a fixed sleep. A sleep
  // long enough to pass reliably is also long enough to pass when the
  // scheduler never ran, so the timeout has to end in a failing assertion.
  const bool merged = WaitFor([&store] { return store.DeltaSize() == 0; }, std::chrono::seconds(10));
  scheduler.Stop();

  ASSERT_TRUE(merged) << "waited for DeltaSize()==0; observed DeltaSize=" << store.DeltaSize()
                      << " MainSize=" << store.MainSize() << " NeedsMerge=" << store.NeedsMerge();
  EXPECT_EQ(store.DeltaSize(), 0U);
  EXPECT_EQ(store.MainSize(), kVectorCount);
  EXPECT_FALSE(store.NeedsMerge());
}

TEST(MergeSchedulerTest, DoubleStartIsHarmless) {
  TieredVectorStore::Config cfg = DefaultConfig();
  TieredVectorStore store(cfg);

  MergeScheduler scheduler(FastSchedulerConfig());
  scheduler.Start(&store);
  scheduler.Start(&store);  // Should not crash or create extra threads
  EXPECT_TRUE(scheduler.IsRunning());
  scheduler.Stop();
  EXPECT_FALSE(scheduler.IsRunning());
}

TEST(MergeSchedulerTest, DestructorStopsThread) {
  constexpr uint32_t kDim = 4;
  constexpr uint32_t kVectorCount = 6;

  TieredVectorStore::Config cfg = DefaultConfig();
  cfg.delta_merge_threshold = 5;
  TieredVectorStore store(cfg);

  {
    MergeScheduler scheduler(FastSchedulerConfig());
    scheduler.Start(&store);
    EXPECT_TRUE(scheduler.IsRunning());
  }

  // A destructor that failed to join would leave a worker still watching the
  // store, so a delta seeded past the threshold after destruction must survive.
  for (uint32_t i = 0; i < kVectorCount; ++i) {
    store.Add("v" + std::to_string(i), MakeAxisVector(kDim, i % kDim));
  }
  ASSERT_TRUE(store.NeedsMerge());
  EXPECT_FALSE(WaitFor([&store] { return store.DeltaSize() == 0; }, std::chrono::seconds(3)))
      << "a worker thread outlived the scheduler and merged the delta: DeltaSize=" << store.DeltaSize()
      << " MainSize=" << store.MainSize();
  EXPECT_EQ(store.DeltaSize(), kVectorCount);
  EXPECT_EQ(store.MainSize(), 0U);
}

TEST(MergeSchedulerTest, SearchesStayConsistentWhileTheSchedulerMerges) {
  constexpr uint32_t kDim = 8;
  constexpr uint32_t kSeedCount = 40;
  constexpr uint32_t kTopK = 5;
  constexpr int kReaders = 4;
  constexpr uint32_t kWriteRounds = 3;
  constexpr uint32_t kWritesPerRound = 15;

  TieredVectorStore::Config cfg = DefaultConfig();
  cfg.delta_merge_threshold = 10;
  TieredVectorStore store(cfg);

  std::mt19937 rng(7);
  std::unordered_set<std::string> known_ids;
  for (uint32_t i = 0; i < kSeedCount; ++i) {
    const std::string id = "seed" + std::to_string(i);
    ASSERT_TRUE(store.Add(id, MakeRandomVector(kDim, rng)).has_value());
    known_ids.insert(id);
  }

  MergeScheduler scheduler(FastSchedulerConfig());
  scheduler.Start(&store);

  // Search takes a shared lock and the merge takes the exclusive one, so every
  // search observes one tier state, never a half-applied merge. Each of the
  // properties below is what "one tier state" means for a result set: the
  // store only grows during this test, so a full page is always available; an
  // entry in flight from delta to main must appear once, not twice or zero
  // times; and the merged ranking must stay ordered.
  //
  // Scope of this case: it detects a merge that loses, duplicates, or
  // misorders entries while readers are running. It does not detect an absent
  // lock as such -- a data race that happens to leave the observable result set
  // intact passes here. Race detection for this path belongs to a
  // ThreadSanitizer run, which this case does not replace.
  std::atomic<bool> stop{false};
  std::atomic<int> searches{0};
  std::atomic<int> short_pages{0};
  std::atomic<int> duplicate_ids{0};
  std::atomic<int> unknown_ids{0};
  std::atomic<int> unordered_scores{0};

  std::vector<std::thread> readers;
  readers.reserve(kReaders);
  for (int reader = 0; reader < kReaders; ++reader) {
    readers.emplace_back([&, reader] {
      std::mt19937 local_rng(static_cast<uint32_t>(reader) + 1);
      while (!stop.load(std::memory_order_acquire)) {
        auto query = MakeRandomVector(kDim, local_rng);
        auto results = store.Search(query.data(), kTopK);
        searches.fetch_add(1, std::memory_order_relaxed);

        if (results.size() != kTopK) {
          short_pages.fetch_add(1, std::memory_order_relaxed);
        }
        std::unordered_set<std::string> seen;
        for (size_t i = 0; i < results.size(); ++i) {
          if (!seen.insert(results[i].id).second) {
            duplicate_ids.fetch_add(1, std::memory_order_relaxed);
          }
          if (known_ids.count(results[i].id) == 0) {
            unknown_ids.fetch_add(1, std::memory_order_relaxed);
          }
          if (i > 0 && results[i].score > results[i - 1].score) {
            unordered_scores.fetch_add(1, std::memory_order_relaxed);
          }
        }
      }
    });
  }

  // Keep the delta above the merge threshold so the scheduler keeps working
  // underneath the readers rather than idling after a single merge. New IDs
  // are published to known_ids before the store so a reader can never see an
  // ID that is not yet in the set.
  for (uint32_t round = 0; round < kWriteRounds; ++round) {
    for (uint32_t i = 0; i < kWritesPerRound; ++i) {
      const std::string id = "round" + std::to_string(round) + "_" + std::to_string(i);
      known_ids.insert(id);
      store.Add(id, MakeRandomVector(kDim, rng));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
  }

  stop.store(true, std::memory_order_release);
  for (auto& reader : readers) {
    reader.join();
  }
  scheduler.Stop();

  EXPECT_GT(searches.load(), 0);
  EXPECT_EQ(short_pages.load(), 0) << "a search returned fewer than top_k results from an always-growing store";
  EXPECT_EQ(duplicate_ids.load(), 0) << "an entry in flight from delta to main was returned twice";
  EXPECT_EQ(unknown_ids.load(), 0) << "a search returned an ID that was never stored";
  EXPECT_EQ(unordered_scores.load(), 0) << "merged results were not ordered by descending score";
  EXPECT_EQ(store.MainSize() + store.DeltaSize(), known_ids.size());
}

}  // namespace
}  // namespace nvecd::vectors
