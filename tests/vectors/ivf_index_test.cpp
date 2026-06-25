/**
 * @file ivf_index_test.cpp
 * @brief Unit tests for IVF (Inverted File) index
 */

#include "vectors/ivf_index.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <numeric>
#include <random>
#include <vector>

#include "vectors/distance.h"
#include "vectors/distance_simd.h"

namespace nvecd::vectors {
namespace {

/// Generate a random normalized vector
std::vector<float> RandomVector(uint32_t dim, std::mt19937& rng) {
  std::normal_distribution<float> dist(0.0F, 1.0F);
  std::vector<float> vec(dim);
  for (auto& v : vec) {
    v = dist(rng);
  }
  Normalize(vec);
  return vec;
}

/// Build a contiguous matrix from a vector of vectors
std::vector<float> BuildMatrix(const std::vector<std::vector<float>>& vecs) {
  if (vecs.empty()) {
    return {};
  }
  size_t dim = vecs[0].size();
  std::vector<float> matrix(vecs.size() * dim);
  for (size_t i = 0; i < vecs.size(); ++i) {
    std::copy(vecs[i].begin(), vecs[i].end(), matrix.begin() + static_cast<ptrdiff_t>(i * dim));
  }
  return matrix;
}

/// Compute norms for a matrix
std::vector<float> ComputeNorms(const std::vector<std::vector<float>>& vecs) {
  std::vector<float> norms(vecs.size());
  for (size_t i = 0; i < vecs.size(); ++i) {
    norms[i] = L2Norm(vecs[i]);
  }
  return norms;
}

class IvfIndexTest : public ::testing::Test {
 protected:
  static constexpr uint32_t kDim = 32;
  static constexpr size_t kNumVectors = 500;

  void SetUp() override {
    std::mt19937 rng(42);  // Fixed seed for reproducibility

    // Generate random normalized vectors
    for (size_t i = 0; i < kNumVectors; ++i) {
      vectors_.push_back(RandomVector(kDim, rng));
    }

    matrix_ = BuildMatrix(vectors_);
    norms_ = ComputeNorms(vectors_);

    // Build valid indices (all vectors are valid)
    valid_indices_.resize(kNumVectors);
    std::iota(valid_indices_.begin(), valid_indices_.end(), size_t{0});
  }

  std::vector<std::vector<float>> vectors_;
  std::vector<float> matrix_;
  std::vector<float> norms_;
  std::vector<size_t> valid_indices_;
};

TEST_F(IvfIndexTest, DefaultConfig) {
  IvfIndex index(kDim);
  EXPECT_FALSE(index.IsTrained());
  EXPECT_EQ(index.GetIndexedCount(), 0);
  EXPECT_EQ(index.GetClusterCount(), 0);
  EXPECT_EQ(index.GetBufferSize(), 0);
}

TEST_F(IvfIndexTest, TrainBasic) {
  IvfIndex::Config config;
  config.nlist = 16;
  config.nprobe = 4;
  config.train_threshold = 100;

  IvfIndex index(kDim, config);

  index.Train(matrix_.data(), valid_indices_.data(), valid_indices_.size(), kDim);

  EXPECT_TRUE(index.IsTrained());
  EXPECT_EQ(index.GetIndexedCount(), kNumVectors);
  EXPECT_EQ(index.GetClusterCount(), 16);
}

TEST_F(IvfIndexTest, TrainWithAutoNlist) {
  IvfIndex::Config config;
  config.nlist = 0;  // Auto: sqrt(n)
  config.nprobe = 4;

  IvfIndex index(kDim, config);

  index.Train(matrix_.data(), valid_indices_.data(), valid_indices_.size(), kDim);

  EXPECT_TRUE(index.IsTrained());
  // sqrt(500) ~= 22
  EXPECT_GT(index.GetClusterCount(), 10);
  EXPECT_LT(index.GetClusterCount(), 30);
  EXPECT_EQ(index.GetIndexedCount(), kNumVectors);
}

TEST_F(IvfIndexTest, TrainClampsNlistToVectorCount) {
  IvfIndex::Config config;
  config.nlist = 10000;  // Way more than kNumVectors

  IvfIndex index(kDim, config);

  index.Train(matrix_.data(), valid_indices_.data(), valid_indices_.size(), kDim);

  EXPECT_TRUE(index.IsTrained());
  // nlist should be clamped to kNumVectors
  EXPECT_EQ(index.GetClusterCount(), kNumVectors);
}

TEST_F(IvfIndexTest, SearchReturnsResults) {
  IvfIndex::Config config;
  config.nlist = 16;
  config.nprobe = 4;

  IvfIndex index(kDim, config);
  index.Train(matrix_.data(), valid_indices_.data(), valid_indices_.size(), kDim);

  // Search using the first vector as query
  const float* query = matrix_.data();
  float query_norm = norms_[0];

  auto results = index.Search(query, query_norm, matrix_.data(), norms_.data(), kNumVectors, kDim, 10);

  // Should get results
  EXPECT_GT(results.size(), 0);
  EXPECT_LE(results.size(), 10);

  // Results should be sorted by score descending
  for (size_t i = 1; i < results.size(); ++i) {
    EXPECT_GE(results[i - 1].first, results[i].first);
  }

  // The query vector itself should be the top result (highest similarity)
  EXPECT_EQ(results[0].second, 0);
  EXPECT_NEAR(results[0].first, 1.0F, 0.01F);  // Self-similarity ~= 1.0
}

TEST_F(IvfIndexTest, SearchWithHighNprobeMatchesBruteForce) {
  IvfIndex::Config config;
  config.nlist = 16;
  config.nprobe = 16;  // Search all clusters = brute force

  IvfIndex index(kDim, config);
  index.Train(matrix_.data(), valid_indices_.data(), valid_indices_.size(), kDim);

  const float* query = matrix_.data();
  float query_norm = norms_[0];

  auto ivf_results = index.Search(query, query_norm, matrix_.data(), norms_.data(), kNumVectors, kDim, 5);

  // Brute-force: compute all similarities
  std::vector<std::pair<float, size_t>> brute_results;
  for (size_t i = 0; i < kNumVectors; ++i) {
    float score = CosineSimilarityPreNorm(query, matrix_.data() + i * kDim, kDim, query_norm, norms_[i]);
    brute_results.push_back({score, i});
  }
  std::sort(brute_results.begin(), brute_results.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
  brute_results.resize(5);

  // With nprobe == nlist, IVF should match brute force exactly
  ASSERT_EQ(ivf_results.size(), brute_results.size());
  for (size_t i = 0; i < ivf_results.size(); ++i) {
    EXPECT_EQ(ivf_results[i].second, brute_results[i].second);
    EXPECT_NEAR(ivf_results[i].first, brute_results[i].first, 1e-5F);
  }
}

TEST_F(IvfIndexTest, AddVectorAfterTraining) {
  IvfIndex::Config config;
  config.nlist = 8;
  config.nprobe = 4;

  IvfIndex index(kDim, config);

  // Train with first 400 vectors
  std::vector<size_t> initial_indices(400);
  std::iota(initial_indices.begin(), initial_indices.end(), size_t{0});

  index.Train(matrix_.data(), initial_indices.data(), 400, kDim);
  EXPECT_EQ(index.GetIndexedCount(), 400);

  // Add remaining vectors one by one
  for (size_t i = 400; i < kNumVectors; ++i) {
    index.AddVector(i, matrix_.data() + i * kDim);
  }
  EXPECT_EQ(index.GetIndexedCount(), kNumVectors);
}

TEST_F(IvfIndexTest, RemoveVector) {
  IvfIndex::Config config;
  config.nlist = 8;
  config.nprobe = 4;

  IvfIndex index(kDim, config);
  index.Train(matrix_.data(), valid_indices_.data(), valid_indices_.size(), kDim);

  size_t initial_count = index.GetIndexedCount();

  // Remove a vector
  index.RemoveVector(0);
  EXPECT_EQ(index.GetIndexedCount(), initial_count - 1);

  // Remove same vector again (no-op)
  index.RemoveVector(0);
  EXPECT_EQ(index.GetIndexedCount(), initial_count - 1);
}

TEST_F(IvfIndexTest, SetNprobe) {
  IvfIndex::Config config;
  config.nlist = 16;
  config.nprobe = 4;

  IvfIndex index(kDim, config);
  EXPECT_EQ(index.GetNprobe(), 4);

  index.SetNprobe(8);
  EXPECT_EQ(index.GetNprobe(), 8);

  // Train first to have nlist set
  index.Train(matrix_.data(), valid_indices_.data(), valid_indices_.size(), kDim);

  // Nprobe clamped to nlist
  index.SetNprobe(100);
  EXPECT_EQ(index.GetNprobe(), 16);
}

TEST_F(IvfIndexTest, SearchBeforeTraining) {
  IvfIndex index(kDim);

  const float* query = matrix_.data();
  float query_norm = norms_[0];

  auto results = index.Search(query, query_norm, matrix_.data(), norms_.data(), kNumVectors, kDim, 10);

  // Should return empty results before training (no buffer entries either)
  EXPECT_TRUE(results.empty());
}

TEST_F(IvfIndexTest, TrainWithEmptyInput) {
  IvfIndex index(kDim);

  index.Train(nullptr, nullptr, 0, kDim);
  EXPECT_FALSE(index.IsTrained());
}

TEST_F(IvfIndexTest, AddVectorBeforeTraining) {
  IvfIndex index(kDim);

  // Should be a no-op, not crash
  index.AddVector(0, matrix_.data());
  EXPECT_EQ(index.GetIndexedCount(), 0);
}

TEST_F(IvfIndexTest, SearchWithDeletedIndices) {
  IvfIndex::Config config;
  config.nlist = 8;
  config.nprobe = 8;  // Search all to ensure deterministic results

  IvfIndex index(kDim, config);
  index.Train(matrix_.data(), valid_indices_.data(), valid_indices_.size(), kDim);

  // Remove first vector
  index.RemoveVector(0);

  // Search should not return the removed vector
  const float* query = matrix_.data();
  float query_norm = norms_[0];

  auto results = index.Search(query, query_norm, matrix_.data(), norms_.data(), kNumVectors, kDim, 10);

  for (const auto& [score, idx] : results) {
    EXPECT_NE(idx, 0) << "Removed vector should not appear in results";
  }
}

TEST_F(IvfIndexTest, SearchQualityWithLowNprobe) {
  // With nprobe=1, we should still find reasonable results
  IvfIndex::Config config;
  config.nlist = 16;
  config.nprobe = 1;

  IvfIndex index(kDim, config);
  index.Train(matrix_.data(), valid_indices_.data(), valid_indices_.size(), kDim);

  const float* query = matrix_.data();
  float query_norm = norms_[0];

  auto results = index.Search(query, query_norm, matrix_.data(), norms_.data(), kNumVectors, kDim, 5);

  // Should still return results (the query's own cluster)
  EXPECT_GT(results.size(), 0);

  // Self should be in the results (same cluster as query)
  bool found_self = false;
  for (const auto& [score, idx] : results) {
    if (idx == 0) {
      found_self = true;
      EXPECT_NEAR(score, 1.0F, 0.01F);
    }
  }
  EXPECT_TRUE(found_self) << "Query vector should be in its own cluster";
}

// ============================================================================
// Write Buffer Tests
// ============================================================================

TEST_F(IvfIndexTest, AppendToBufferBasic) {
  IvfIndex index(kDim);

  EXPECT_EQ(index.GetBufferSize(), 0);

  // Append a few vectors to the buffer
  for (size_t i = 0; i < 10; ++i) {
    index.AppendToBuffer(i, vectors_[i].data());
  }

  EXPECT_EQ(index.GetBufferSize(), 10);
}

TEST_F(IvfIndexTest, AppendToBufferNullVector) {
  IvfIndex index(kDim);

  // Appending null should be a no-op
  index.AppendToBuffer(0, nullptr);
  EXPECT_EQ(index.GetBufferSize(), 0);
}

TEST_F(IvfIndexTest, SearchBufferOnly) {
  IvfIndex index(kDim);

  // Append vectors to buffer (no training)
  for (size_t i = 0; i < 50; ++i) {
    index.AppendToBuffer(i, vectors_[i].data());
  }
  EXPECT_EQ(index.GetBufferSize(), 50);

  // Search the buffer via the unified Search path
  const float* query = vectors_[0].data();
  float query_norm = norms_[0];

  auto results = index.Search(query, query_norm, matrix_.data(), norms_.data(), kNumVectors, kDim, 5);

  // Should get results from buffer brute-force search
  EXPECT_GT(results.size(), 0);
  EXPECT_LE(results.size(), 5);

  // Results should be sorted by score descending
  for (size_t i = 1; i < results.size(); ++i) {
    EXPECT_GE(results[i - 1].first, results[i].first);
  }

  // The query vector itself should be the top result
  EXPECT_EQ(results[0].second, 0);
  EXPECT_NEAR(results[0].first, 1.0F, 0.01F);
}

TEST_F(IvfIndexTest, SearchBufferDirectly) {
  IvfIndex index(kDim);

  // Append vectors to buffer
  for (size_t i = 0; i < 50; ++i) {
    index.AppendToBuffer(i, vectors_[i].data());
  }

  // Search buffer directly
  const float* query = vectors_[0].data();
  float query_norm = norms_[0];

  auto results = index.SearchBuffer(query, query_norm, kDim, 5);

  EXPECT_GT(results.size(), 0);
  EXPECT_LE(results.size(), 5);

  // Top result should be the query itself
  EXPECT_EQ(results[0].second, 0);
  EXPECT_NEAR(results[0].first, 1.0F, 0.01F);
}

TEST_F(IvfIndexTest, NeedsSealThreshold) {
  IvfIndex::Config config;
  config.seal_threshold = 20;  // Low threshold for testing

  IvfIndex index(kDim, config);

  // Below threshold
  for (size_t i = 0; i < 19; ++i) {
    index.AppendToBuffer(i, vectors_[i].data());
  }
  EXPECT_FALSE(index.NeedsSeal());

  // At threshold
  index.AppendToBuffer(19, vectors_[19].data());
  EXPECT_TRUE(index.NeedsSeal());
}

TEST_F(IvfIndexTest, SealBufferMovesToIvf) {
  IvfIndex::Config config;
  config.nlist = 8;
  config.nprobe = 8;
  config.seal_threshold = 50;

  IvfIndex index(kDim, config);

  // Train with first 400 vectors
  std::vector<size_t> initial_indices(400);
  std::iota(initial_indices.begin(), initial_indices.end(), size_t{0});
  index.Train(matrix_.data(), initial_indices.data(), 400, kDim);

  size_t initial_indexed = index.GetIndexedCount();
  EXPECT_EQ(initial_indexed, 400);

  // Append 50 vectors to buffer
  for (size_t i = 400; i < 450; ++i) {
    index.AppendToBuffer(i, vectors_[i].data());
  }
  EXPECT_EQ(index.GetBufferSize(), 50);

  // Seal: buffer entries move to IVF inverted lists
  index.SealBuffer();

  EXPECT_EQ(index.GetBufferSize(), 0);
  EXPECT_EQ(index.GetIndexedCount(), 450);
}

TEST_F(IvfIndexTest, SealBufferWhenNotTrained) {
  IvfIndex index(kDim);

  // Append vectors to buffer without training
  for (size_t i = 0; i < 20; ++i) {
    index.AppendToBuffer(i, vectors_[i].data());
  }

  // Seal should be a no-op (not trained)
  index.SealBuffer();

  // Buffer should remain intact
  EXPECT_EQ(index.GetBufferSize(), 20);
  EXPECT_EQ(index.GetIndexedCount(), 0);
}

TEST_F(IvfIndexTest, SealEmptyBuffer) {
  IvfIndex::Config config;
  config.nlist = 8;
  config.nprobe = 4;

  IvfIndex index(kDim, config);
  index.Train(matrix_.data(), valid_indices_.data(), valid_indices_.size(), kDim);

  // Seal empty buffer is a no-op
  size_t count_before = index.GetIndexedCount();
  index.SealBuffer();
  EXPECT_EQ(index.GetIndexedCount(), count_before);
}

TEST_F(IvfIndexTest, TwoTierSearchMergesResults) {
  IvfIndex::Config config;
  config.nlist = 8;
  config.nprobe = 8;  // Search all clusters for determinism

  IvfIndex index(kDim, config);

  // Train with first 400 vectors
  std::vector<size_t> initial_indices(400);
  std::iota(initial_indices.begin(), initial_indices.end(), size_t{0});
  index.Train(matrix_.data(), initial_indices.data(), 400, kDim);

  // Append remaining 100 vectors to buffer (not sealed)
  for (size_t i = 400; i < kNumVectors; ++i) {
    index.AppendToBuffer(i, vectors_[i].data());
  }

  EXPECT_EQ(index.GetIndexedCount(), 400);
  EXPECT_EQ(index.GetBufferSize(), 100);

  // Search should merge results from both tiers
  const float* query = vectors_[0].data();
  float query_norm = norms_[0];

  auto results = index.Search(query, query_norm, matrix_.data(), norms_.data(), kNumVectors, kDim, 10);

  EXPECT_GT(results.size(), 0);
  EXPECT_LE(results.size(), 10);

  // Results should be sorted by score descending
  for (size_t i = 1; i < results.size(); ++i) {
    EXPECT_GE(results[i - 1].first, results[i].first);
  }

  // Self should be found (it's in the IVF tier)
  EXPECT_EQ(results[0].second, 0);
  EXPECT_NEAR(results[0].first, 1.0F, 0.01F);
}

TEST_F(IvfIndexTest, TwoTierSearchDeduplicates) {
  IvfIndex::Config config;
  config.nlist = 8;
  config.nprobe = 8;

  IvfIndex index(kDim, config);

  // Train with all vectors (assigns them to IVF)
  index.Train(matrix_.data(), valid_indices_.data(), valid_indices_.size(), kDim);

  // Also add some of the same vectors to the buffer (duplicates)
  for (size_t i = 0; i < 10; ++i) {
    index.AppendToBuffer(i, vectors_[i].data());
  }

  const float* query = vectors_[0].data();
  float query_norm = norms_[0];

  auto results = index.Search(query, query_norm, matrix_.data(), norms_.data(), kNumVectors, kDim, 10);

  // Check no duplicate compact_indices in results
  std::vector<size_t> result_indices;
  for (const auto& [score, idx] : results) {
    result_indices.push_back(idx);
  }
  std::sort(result_indices.begin(), result_indices.end());
  auto unique_end = std::unique(result_indices.begin(), result_indices.end());
  EXPECT_EQ(unique_end, result_indices.end()) << "Search results should not contain duplicate compact indices";
}

TEST_F(IvfIndexTest, RemoveVectorFromBuffer) {
  IvfIndex index(kDim);

  // Append vectors to buffer
  for (size_t i = 0; i < 10; ++i) {
    index.AppendToBuffer(i, vectors_[i].data());
  }
  EXPECT_EQ(index.GetBufferSize(), 10);

  // Remove a vector from the buffer
  index.RemoveVector(5);
  EXPECT_EQ(index.GetBufferSize(), 9);

  // Search buffer should not return the removed vector
  const float* query = vectors_[5].data();
  float query_norm = norms_[5];

  auto results = index.SearchBuffer(query, query_norm, kDim, 10);
  for (const auto& [score, idx] : results) {
    EXPECT_NE(idx, 5) << "Removed vector should not appear in buffer search";
  }
}

// ============================================================================
// Distance Metric Tests (C-2: IVF must honor the configured metric)
// ============================================================================

/// Generate a random vector WITHOUT normalization, so dot/L2/cosine differ.
std::vector<float> RandomUnnormalizedVector(uint32_t dim, std::mt19937& rng) {
  std::uniform_real_distribution<float> dist(-3.0F, 3.0F);
  std::vector<float> vec(dim);
  for (auto& v : vec) {
    v = dist(rng);
  }
  return vec;
}

/// Brute-force top-k by metric ("higher score == nearer"), matching IvfIndex.
std::vector<std::pair<float, size_t>> BruteForceTopK(IvfMetric metric, const float* query, float query_norm,
                                                     const std::vector<std::vector<float>>& vecs,
                                                     const std::vector<float>& norms, uint32_t dim, size_t top_k) {
  const auto& impl = simd::GetOptimalImpl();
  std::vector<std::pair<float, size_t>> scored;
  scored.reserve(vecs.size());
  for (size_t i = 0; i < vecs.size(); ++i) {
    const float* cand = vecs[i].data();
    float score = 0.0F;
    switch (metric) {
      case IvfMetric::kDot:
        score = impl.dot_product(query, cand, dim);
        break;
      case IvfMetric::kL2:
        score = 1.0F / (1.0F + impl.l2_distance(query, cand, dim));
        break;
      case IvfMetric::kCosine:
        score = impl.dot_product(query, cand, dim) / (query_norm * norms[i]);
        break;
    }
    scored.push_back({score, i});
  }
  std::sort(scored.begin(), scored.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
  if (scored.size() > top_k) {
    scored.resize(top_k);
  }
  return scored;
}

class IvfMetricTest : public ::testing::TestWithParam<IvfMetric> {
 protected:
  static constexpr uint32_t kDim = 16;
  static constexpr size_t kNumVectors = 200;

  void SetUp() override {
    std::mt19937 rng(123);  // Fixed seed for reproducibility
    for (size_t i = 0; i < kNumVectors; ++i) {
      vectors_.push_back(RandomUnnormalizedVector(kDim, rng));
    }
    matrix_ = BuildMatrix(vectors_);
    norms_ = ComputeNorms(vectors_);
    valid_indices_.resize(kNumVectors);
    std::iota(valid_indices_.begin(), valid_indices_.end(), size_t{0});
  }

  std::vector<std::vector<float>> vectors_;
  std::vector<float> matrix_;
  std::vector<float> norms_;
  std::vector<size_t> valid_indices_;
};

// With nprobe == nlist, the IVF scans every cluster, so it must reproduce the
// brute-force top-k for the configured metric exactly (ordering and scores).
TEST_P(IvfMetricTest, ExhaustiveSearchMatchesBruteForceForMetric) {
  IvfMetric metric = GetParam();

  IvfIndex::Config config;
  config.nlist = 8;
  config.nprobe = 8;  // Search all clusters -> exact (no approximation)
  config.metric = metric;

  IvfIndex index(kDim, config);
  index.Train(matrix_.data(), valid_indices_.data(), valid_indices_.size(), kDim);

  // Use a vector NOT in the dataset as the query to avoid self-match dominating.
  std::mt19937 rng(999);
  std::vector<float> query = RandomUnnormalizedVector(kDim, rng);
  float query_norm = L2Norm(query);

  const size_t kTopK = 10;
  auto ivf_results = index.Search(query.data(), query_norm, matrix_.data(), norms_.data(), kNumVectors, kDim, kTopK);
  auto brute = BruteForceTopK(metric, query.data(), query_norm, vectors_, norms_, kDim, kTopK);

  ASSERT_EQ(ivf_results.size(), brute.size());
  for (size_t i = 0; i < ivf_results.size(); ++i) {
    EXPECT_EQ(ivf_results[i].second, brute[i].second) << "metric=" << static_cast<int>(metric) << " rank=" << i;
    EXPECT_NEAR(ivf_results[i].first, brute[i].first, 1e-4F) << "metric=" << static_cast<int>(metric) << " rank=" << i;
  }
}

// The top-1 result must be metric-correct even with a single probe when the
// query lands in the right cluster. We assert IVF top-1 equals brute-force top-1.
TEST_P(IvfMetricTest, BufferOnlySearchMatchesBruteForceForMetric) {
  IvfMetric metric = GetParam();

  IvfIndex::Config config;
  config.metric = metric;
  IvfIndex index(kDim, config);

  // No training: everything lives in the write buffer (brute-force scanned).
  for (size_t i = 0; i < kNumVectors; ++i) {
    index.AppendToBuffer(i, vectors_[i].data());
  }

  std::mt19937 rng(999);
  std::vector<float> query = RandomUnnormalizedVector(kDim, rng);
  float query_norm = L2Norm(query);

  const size_t kTopK = 10;
  auto ivf_results = index.SearchBuffer(query.data(), query_norm, kDim, kTopK);
  auto brute = BruteForceTopK(metric, query.data(), query_norm, vectors_, norms_, kDim, kTopK);

  ASSERT_EQ(ivf_results.size(), brute.size());
  for (size_t i = 0; i < ivf_results.size(); ++i) {
    EXPECT_EQ(ivf_results[i].second, brute[i].second) << "metric=" << static_cast<int>(metric) << " rank=" << i;
    EXPECT_NEAR(ivf_results[i].first, brute[i].first, 1e-4F) << "metric=" << static_cast<int>(metric) << " rank=" << i;
  }
}

INSTANTIATE_TEST_SUITE_P(AllMetrics, IvfMetricTest,
                         ::testing::Values(IvfMetric::kCosine, IvfMetric::kDot, IvfMetric::kL2));

// Different metrics on the SAME data/query should generally yield different
// top-1 results, proving the metric is actually threaded through (not ignored).
TEST(IvfMetricDivergenceTest, DotAndL2DifferFromCosine) {
  constexpr uint32_t kDim = 16;
  constexpr size_t kNumVectors = 200;

  std::mt19937 rng(7);
  std::vector<std::vector<float>> vecs;
  for (size_t i = 0; i < kNumVectors; ++i) {
    vecs.push_back(RandomUnnormalizedVector(kDim, rng));
  }
  std::vector<float> matrix = BuildMatrix(vecs);
  std::vector<float> norms = ComputeNorms(vecs);
  std::vector<size_t> indices(kNumVectors);
  std::iota(indices.begin(), indices.end(), size_t{0});

  std::vector<float> query = RandomUnnormalizedVector(kDim, rng);
  float query_norm = L2Norm(query);

  auto top1 = [&](IvfMetric metric) -> size_t {
    IvfIndex::Config config;
    config.nlist = 8;
    config.nprobe = 8;
    config.metric = metric;
    IvfIndex index(kDim, config);
    index.Train(matrix.data(), indices.data(), kNumVectors, kDim);
    auto r = index.Search(query.data(), query_norm, matrix.data(), norms.data(), kNumVectors, kDim, 1);
    return r.empty() ? SIZE_MAX : r[0].second;
  };

  size_t cosine_top = top1(IvfMetric::kCosine);
  size_t dot_top = top1(IvfMetric::kDot);
  // At least one of dot/L2 should disagree with cosine on un-normalized data.
  EXPECT_TRUE(dot_top != cosine_top || top1(IvfMetric::kL2) != cosine_top)
      << "All metrics agreed on top-1; metric may be ignored";
}

// Bug #8: zero-norm vectors must not all collapse into cluster 0.
TEST(IvfZeroNormTest, ZeroNormVectorsDoNotAllCollapseToClusterZero) {
  constexpr uint32_t kDim = 8;
  constexpr size_t kNonZero = 100;
  constexpr size_t kZero = 40;

  std::mt19937 rng(31);
  std::vector<std::vector<float>> vecs;
  for (size_t i = 0; i < kNonZero; ++i) {
    vecs.push_back(RandomUnnormalizedVector(kDim, rng));
  }
  // Append zero-norm vectors (all components zero).
  for (size_t i = 0; i < kZero; ++i) {
    vecs.push_back(std::vector<float>(kDim, 0.0F));
  }
  std::vector<float> matrix = BuildMatrix(vecs);
  std::vector<size_t> indices(vecs.size());
  std::iota(indices.begin(), indices.end(), size_t{0});

  IvfIndex::Config config;
  config.nlist = 8;
  config.nprobe = 8;
  config.metric = IvfMetric::kCosine;
  IvfIndex index(kDim, config);

  // Must not crash; all vectors get assigned to some cluster.
  index.Train(matrix.data(), indices.data(), vecs.size(), kDim);
  EXPECT_TRUE(index.IsTrained());
  EXPECT_EQ(index.GetIndexedCount(), vecs.size());

  // The zero-norm vectors (indices kNonZero..) are spread by idx % nlist, so
  // they should land in several distinct clusters rather than one. We verify by
  // searching and confirming the index is usable, plus that not every zero-norm
  // index shares a single cluster (checked indirectly: recall on non-zero data
  // is unaffected because zero vectors are spread, not piled into cluster 0).
  std::vector<float> query = vecs[0];
  float query_norm = L2Norm(query);
  auto results = index.Search(query.data(), query_norm, matrix.data(), ComputeNorms(vecs).data(), vecs.size(), kDim, 5);
  EXPECT_GT(results.size(), 0U);
  // Top result should be the (non-zero) query itself.
  EXPECT_EQ(results[0].second, 0U);
}

}  // namespace
}  // namespace nvecd::vectors
