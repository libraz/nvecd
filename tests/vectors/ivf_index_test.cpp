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
    std::copy(vecs[i].begin(), vecs[i].end(),
              matrix.begin() + static_cast<ptrdiff_t>(i * dim));
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
}

TEST_F(IvfIndexTest, TrainBasic) {
  IvfIndex::Config config;
  config.nlist = 16;
  config.nprobe = 4;
  config.train_threshold = 100;

  IvfIndex index(kDim, config);

  index.Train(matrix_.data(), valid_indices_.data(),
              valid_indices_.size(), kDim);

  EXPECT_TRUE(index.IsTrained());
  EXPECT_EQ(index.GetIndexedCount(), kNumVectors);
  EXPECT_EQ(index.GetClusterCount(), 16);
}

TEST_F(IvfIndexTest, TrainWithAutoNlist) {
  IvfIndex::Config config;
  config.nlist = 0;  // Auto: sqrt(n)
  config.nprobe = 4;

  IvfIndex index(kDim, config);

  index.Train(matrix_.data(), valid_indices_.data(),
              valid_indices_.size(), kDim);

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

  index.Train(matrix_.data(), valid_indices_.data(),
              valid_indices_.size(), kDim);

  EXPECT_TRUE(index.IsTrained());
  // nlist should be clamped to kNumVectors
  EXPECT_EQ(index.GetClusterCount(), kNumVectors);
}

TEST_F(IvfIndexTest, SearchReturnsResults) {
  IvfIndex::Config config;
  config.nlist = 16;
  config.nprobe = 4;

  IvfIndex index(kDim, config);
  index.Train(matrix_.data(), valid_indices_.data(),
              valid_indices_.size(), kDim);

  // Search using the first vector as query
  const float* query = matrix_.data();
  float query_norm = norms_[0];

  auto results = index.Search(query, query_norm, matrix_.data(), norms_.data(),
                              kNumVectors, kDim, 10);

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
  index.Train(matrix_.data(), valid_indices_.data(),
              valid_indices_.size(), kDim);

  const float* query = matrix_.data();
  float query_norm = norms_[0];

  auto ivf_results = index.Search(query, query_norm, matrix_.data(),
                                  norms_.data(), kNumVectors, kDim, 5);

  // Brute-force: compute all similarities
  std::vector<std::pair<float, size_t>> brute_results;
  for (size_t i = 0; i < kNumVectors; ++i) {
    float score = CosineSimilarityPreNorm(
        query, matrix_.data() + i * kDim, kDim, query_norm, norms_[i]);
    brute_results.push_back({score, i});
  }
  std::sort(brute_results.begin(), brute_results.end(),
            [](const auto& a, const auto& b) { return a.first > b.first; });
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
  index.Train(matrix_.data(), valid_indices_.data(),
              valid_indices_.size(), kDim);

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
  index.Train(matrix_.data(), valid_indices_.data(),
              valid_indices_.size(), kDim);

  // Nprobe clamped to nlist
  index.SetNprobe(100);
  EXPECT_EQ(index.GetNprobe(), 16);
}

TEST_F(IvfIndexTest, SearchBeforeTraining) {
  IvfIndex index(kDim);

  const float* query = matrix_.data();
  float query_norm = norms_[0];

  auto results = index.Search(query, query_norm, matrix_.data(), norms_.data(),
                              kNumVectors, kDim, 10);

  // Should return empty results before training
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
  index.Train(matrix_.data(), valid_indices_.data(),
              valid_indices_.size(), kDim);

  // Remove first vector
  index.RemoveVector(0);

  // Search should not return the removed vector
  const float* query = matrix_.data();
  float query_norm = norms_[0];

  auto results = index.Search(query, query_norm, matrix_.data(), norms_.data(),
                              kNumVectors, kDim, 10);

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
  index.Train(matrix_.data(), valid_indices_.data(),
              valid_indices_.size(), kDim);

  const float* query = matrix_.data();
  float query_norm = norms_[0];

  auto results = index.Search(query, query_norm, matrix_.data(), norms_.data(),
                              kNumVectors, kDim, 5);

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

}  // namespace
}  // namespace nvecd::vectors
