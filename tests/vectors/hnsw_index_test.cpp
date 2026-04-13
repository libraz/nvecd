/**
 * @file hnsw_index_test.cpp
 * @brief Tests for HnswIndex
 */

#include "vectors/hnsw_index.h"

#include <algorithm>
#include <cmath>
#include <random>
#include <sstream>
#include <unordered_set>
#include <vector>

#include <gtest/gtest.h>

#include "vectors/distance.h"

namespace nvecd::vectors {
namespace {

class HnswIndexTest : public ::testing::Test {
 protected:
  static constexpr uint32_t kDim = 8;

  void SetUp() override {
    HnswIndex::Config config;
    config.m = 8;
    config.ef_construction = 50;
    config.ef_search = 20;
    index_ = std::make_unique<HnswIndex>(kDim, CosineDistanceRaw, config);
  }

  /// Generate a random unit vector
  std::vector<float> RandomVector(std::mt19937& rng) {
    std::normal_distribution<float> dist(0.0F, 1.0F);
    std::vector<float> vec(kDim);
    float norm = 0.0F;
    for (auto& v : vec) {
      v = dist(rng);
      norm += v * v;
    }
    norm = std::sqrt(norm);
    for (auto& v : vec) {
      v /= norm;
    }
    return vec;
  }

  std::unique_ptr<HnswIndex> index_;
};

// ============================================================================
// Basic operations
// ============================================================================

TEST_F(HnswIndexTest, EmptyIndex) {
  EXPECT_EQ(index_->Size(), 0U);
  auto results = index_->Search(std::vector<float>(kDim, 1.0F).data(), 10);
  EXPECT_TRUE(results.empty());
}

TEST_F(HnswIndexTest, SingleVector) {
  std::vector<float> vec(kDim, 0.0F);
  vec[0] = 1.0F;
  index_->Add(0, vec.data());

  EXPECT_EQ(index_->Size(), 1U);

  auto results = index_->Search(vec.data(), 5);
  ASSERT_EQ(results.size(), 1U);
  EXPECT_EQ(results[0].first, 0U);
  EXPECT_NEAR(results[0].second, 1.0F, 0.01F);
}

TEST_F(HnswIndexTest, AddMultipleVectors) {
  std::mt19937 rng(42);
  constexpr uint32_t kCount = 100;

  for (uint32_t i = 0; i < kCount; ++i) {
    auto vec = RandomVector(rng);
    index_->Add(i, vec.data());
  }

  EXPECT_EQ(index_->Size(), kCount);
}

TEST_F(HnswIndexTest, SearchReturnsTopK) {
  std::mt19937 rng(42);
  constexpr uint32_t kCount = 50;

  std::vector<std::vector<float>> vecs;
  for (uint32_t i = 0; i < kCount; ++i) {
    auto vec = RandomVector(rng);
    vecs.push_back(vec);
    index_->Add(i, vec.data());
  }

  auto results = index_->Search(vecs[0].data(), 5);
  ASSERT_LE(results.size(), 5U);
  ASSERT_GE(results.size(), 1U);

  // First result should be the query itself (or very similar)
  EXPECT_EQ(results[0].first, 0U);

  // Results should be sorted by score descending
  for (size_t i = 1; i < results.size(); ++i) {
    EXPECT_GE(results[i - 1].second, results[i].second);
  }
}

TEST_F(HnswIndexTest, SearchExcludesDeleted) {
  std::vector<float> v1(kDim, 0.0F);
  v1[0] = 1.0F;
  std::vector<float> v2(kDim, 0.0F);
  v2[0] = 0.99F;
  v2[1] = 0.14F;

  index_->Add(0, v1.data());
  index_->Add(1, v2.data());

  // Delete the closest match
  index_->MarkDeleted(0);
  EXPECT_EQ(index_->Size(), 1U);

  auto results = index_->Search(v1.data(), 5);
  ASSERT_EQ(results.size(), 1U);
  EXPECT_EQ(results[0].first, 1U);
}

TEST_F(HnswIndexTest, MarkDeletedTwiceIsHarmless) {
  std::vector<float> vec(kDim, 1.0F);
  index_->Add(0, vec.data());
  EXPECT_EQ(index_->Size(), 1U);

  index_->MarkDeleted(0);
  EXPECT_EQ(index_->Size(), 0U);

  index_->MarkDeleted(0);  // No crash, no underflow
  EXPECT_EQ(index_->Size(), 0U);
}

TEST_F(HnswIndexTest, MarkDeletedNonExistent) {
  // Should not crash
  index_->MarkDeleted(999);
  EXPECT_EQ(index_->Size(), 0U);
}

// ============================================================================
// Recall quality
// ============================================================================

TEST_F(HnswIndexTest, RecallAtK10) {
  std::mt19937 rng(123);
  constexpr uint32_t kCount = 500;
  constexpr uint32_t kTopK = 10;
  constexpr uint32_t kQueries = 20;

  std::vector<std::vector<float>> vecs;
  for (uint32_t i = 0; i < kCount; ++i) {
    auto vec = RandomVector(rng);
    vecs.push_back(vec);
    index_->Add(i, vec.data());
  }

  // Compute recall against brute-force
  float total_recall = 0.0F;
  for (uint32_t q = 0; q < kQueries; ++q) {
    auto query = RandomVector(rng);

    // Brute-force ground truth
    std::vector<std::pair<float, uint32_t>> bf_results;
    for (uint32_t i = 0; i < kCount; ++i) {
      float score = CosineDistanceRaw(query.data(), vecs[i].data(), kDim);
      bf_results.push_back({score, i});
    }
    std::sort(bf_results.begin(), bf_results.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });

    std::unordered_set<uint32_t> gt_set;
    for (uint32_t i = 0; i < kTopK && i < kCount; ++i) {
      gt_set.insert(bf_results[i].second);
    }

    // HNSW search
    auto hnsw_results = index_->Search(query.data(), kTopK);

    uint32_t hits = 0;
    for (const auto& [idx, score] : hnsw_results) {
      if (gt_set.count(idx) > 0) {
        hits++;
      }
    }

    total_recall += static_cast<float>(hits) / static_cast<float>(kTopK);
  }

  float avg_recall = total_recall / static_cast<float>(kQueries);
  EXPECT_GE(avg_recall, 0.80F)
      << "Average recall@" << kTopK << " = " << avg_recall
      << " (expected >= 0.80)";
}

// ============================================================================
// Rebuild
// ============================================================================

TEST_F(HnswIndexTest, RebuildFromMatrix) {
  constexpr uint32_t kCount = 20;
  std::mt19937 rng(77);

  std::vector<float> matrix(kCount * kDim);
  for (auto& v : matrix) {
    v = std::uniform_real_distribution<float>(-1.0F, 1.0F)(rng);
  }

  index_->Rebuild(matrix.data(), kCount, kDim);
  EXPECT_EQ(index_->Size(), kCount);

  // Search should work
  auto results = index_->Search(matrix.data(), 5);
  ASSERT_FALSE(results.empty());
  // First result should be compact_index 0 (the query vector)
  EXPECT_EQ(results[0].first, 0U);
}

// ============================================================================
// Serialization
// ============================================================================

TEST_F(HnswIndexTest, SerializeDeserializeRoundTrip) {
  std::mt19937 rng(99);
  constexpr uint32_t kCount = 30;

  std::vector<std::vector<float>> vecs;
  for (uint32_t i = 0; i < kCount; ++i) {
    auto vec = RandomVector(rng);
    vecs.push_back(vec);
    index_->Add(i, vec.data());
  }

  // Delete a few
  index_->MarkDeleted(5);
  index_->MarkDeleted(15);

  // Serialize
  std::stringstream ss;
  auto ser_result = index_->Serialize(ss);
  ASSERT_TRUE(ser_result.has_value());

  // Deserialize into a new index
  HnswIndex::Config config;
  config.m = 8;
  config.ef_construction = 50;
  config.ef_search = 20;
  HnswIndex restored(kDim, CosineDistanceRaw, config);
  auto deser_result = restored.Deserialize(ss);
  ASSERT_TRUE(deser_result.has_value());

  EXPECT_EQ(restored.Size(), kCount - 2);
  EXPECT_EQ(restored.GetNodeCount(), kCount);

  // Search should return same results
  auto query = RandomVector(rng);
  auto orig_results = index_->Search(query.data(), 10);
  auto rest_results = restored.Search(query.data(), 10);

  ASSERT_EQ(orig_results.size(), rest_results.size());
  for (size_t i = 0; i < orig_results.size(); ++i) {
    EXPECT_EQ(orig_results[i].first, rest_results[i].first);
    EXPECT_NEAR(orig_results[i].second, rest_results[i].second, 1e-5F);
  }
}

TEST_F(HnswIndexTest, DeserializeInvalidMagic) {
  std::stringstream ss;
  uint32_t bad_magic = 0x12345678;
  ss.write(reinterpret_cast<const char*>(&bad_magic), sizeof(bad_magic));
  uint32_t version = 1;
  ss.write(reinterpret_cast<const char*>(&version), sizeof(version));

  HnswIndex::Config config;
  HnswIndex restored(kDim, CosineDistanceRaw, config);
  auto result = restored.Deserialize(ss);
  EXPECT_FALSE(result.has_value());
}

// ============================================================================
// Graph structure
// ============================================================================

TEST_F(HnswIndexTest, MaxLevelGrowsWithInsertions) {
  std::mt19937 rng(42);

  // With enough insertions, max_level should be > 0
  for (uint32_t i = 0; i < 200; ++i) {
    auto vec = RandomVector(rng);
    index_->Add(i, vec.data());
  }

  EXPECT_GT(index_->GetMaxLevel(), 0U);
}

TEST_F(HnswIndexTest, TopKExceedsCount) {
  std::vector<float> vec(kDim, 1.0F);
  index_->Add(0, vec.data());
  index_->Add(1, vec.data());

  auto results = index_->Search(vec.data(), 100);
  EXPECT_EQ(results.size(), 2U);
}

// ============================================================================
// Different distance metrics
// ============================================================================

TEST_F(HnswIndexTest, DotProductMetric) {
  HnswIndex::Config config;
  config.m = 8;
  config.ef_construction = 50;
  config.ef_search = 20;
  HnswIndex dot_index(kDim, DotProductRaw, config);

  std::vector<float> v1(kDim, 0.0F);
  v1[0] = 1.0F;
  std::vector<float> v2(kDim, 0.0F);
  v2[0] = 0.5F;
  v2[1] = 0.5F;

  dot_index.Add(0, v1.data());
  dot_index.Add(1, v2.data());

  auto results = dot_index.Search(v1.data(), 2);
  ASSERT_EQ(results.size(), 2U);
  EXPECT_EQ(results[0].first, 0U);  // Self is most similar
}

TEST_F(HnswIndexTest, L2SimilarityMetric) {
  HnswIndex::Config config;
  config.m = 8;
  config.ef_construction = 50;
  config.ef_search = 20;
  HnswIndex l2_index(kDim, L2SimilarityRaw, config);

  std::vector<float> v1(kDim, 0.0F);
  v1[0] = 1.0F;
  std::vector<float> v2(kDim, 0.0F);
  v2[0] = 0.9F;
  std::vector<float> v3(kDim, 0.0F);
  v3[0] = -1.0F;

  l2_index.Add(0, v1.data());
  l2_index.Add(1, v2.data());
  l2_index.Add(2, v3.data());

  auto results = l2_index.Search(v1.data(), 3);
  ASSERT_EQ(results.size(), 3U);
  EXPECT_EQ(results[0].first, 0U);  // Self
  EXPECT_EQ(results[1].first, 1U);  // Closest
  EXPECT_EQ(results[2].first, 2U);  // Farthest
}

// ============================================================================
// SetEfSearch
// ============================================================================

TEST_F(HnswIndexTest, SetEfSearch) {
  EXPECT_EQ(index_->GetEfSearch(), 20U);
  index_->SetEfSearch(100);
  EXPECT_EQ(index_->GetEfSearch(), 100U);
}

}  // namespace
}  // namespace nvecd::vectors
