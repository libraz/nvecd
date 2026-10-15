/**
 * @file hnsw_visited_marks_test.cpp
 * @brief HNSW searches reuse a per-thread visited-marks buffer
 */

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <iostream>
#include <memory>
#include <random>
#include <thread>
#include <vector>

#include "vectors/distance.h"
#include "vectors/hnsw_index.h"

namespace nvecd::vectors {
namespace {

std::vector<float> RandomUnitVector(std::mt19937& rng, uint32_t dim) {
  std::normal_distribution<float> dist(0.0F, 1.0F);
  std::vector<float> vec(dim);
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

class HnswVisitedMarksTest : public ::testing::Test {
 protected:
  void Build(size_t count, uint32_t dim) {
    dim_ = dim;
    HnswIndex::Config config;
    config.m = 8;
    config.ef_construction = 50;
    config.ef_search = 40;
    index_ = std::make_unique<HnswIndex>(dim, CosineDistanceRaw, config);
    std::mt19937 rng(11);
    for (size_t i = 0; i < count; ++i) {
      vectors_.push_back(RandomUnitVector(rng, dim));
      index_->Add(static_cast<uint32_t>(i), vectors_.back().data());
    }
  }

  uint32_t dim_ = 0;
  std::vector<std::vector<float>> vectors_;
  std::unique_ptr<HnswIndex> index_;
};

TEST_F(HnswVisitedMarksTest, RepeatedSearchesReuseTheBuffer) {
  Build(500, 16);
  ASSERT_FALSE(index_->Search(vectors_[0].data(), 5).empty());  // Warm this thread's buffer

  const uint64_t allocations = HnswIndex::VisitedBufferAllocations();
  for (size_t i = 0; i < 300; ++i) {
    ASSERT_FALSE(index_->Search(vectors_[i % vectors_.size()].data(), 5).empty());
  }
  EXPECT_EQ(HnswIndex::VisitedBufferAllocations(), allocations);
}

TEST_F(HnswVisitedMarksTest, EverySearchStartsWithCleanMarks) {
  Build(300, 16);
  // A stale mark from an earlier search would hide nodes from a later one, so
  // each stored vector must still find itself.
  for (size_t round = 0; round < 3; ++round) {
    for (size_t i = 0; i < vectors_.size(); ++i) {
      const auto results = index_->Search(vectors_[i].data(), 1);
      ASSERT_EQ(results.size(), 1U);
      EXPECT_EQ(results[0].first, i);
    }
  }
}

TEST_F(HnswVisitedMarksTest, ConcurrentSearchesDoNotShareMarks) {
  Build(400, 16);
  std::vector<std::thread> threads;
  std::vector<size_t> misses(4, 0);
  for (size_t t = 0; t < misses.size(); ++t) {
    threads.emplace_back([this, t, &misses] {
      for (size_t i = t; i < vectors_.size(); i += 4) {
        for (int repeat = 0; repeat < 5; ++repeat) {
          const auto results = index_->Search(vectors_[i].data(), 1);
          if (results.size() != 1 || results[0].first != i) {
            ++misses[t];
          }
        }
      }
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }
  for (size_t miss : misses) {
    EXPECT_EQ(miss, 0U);
  }
}

// Run: <test binary> --gtest_also_run_disabled_tests --gtest_filter='HnswVisitedMarksTest.DISABLED_SearchLatency'
TEST_F(HnswVisitedMarksTest, DISABLED_SearchLatency) {
  Build(20000, 32);
  constexpr size_t kSearches = 20000;
  std::mt19937 rng(5);
  std::vector<std::vector<float>> queries;
  for (size_t i = 0; i < 256; ++i) {
    queries.push_back(RandomUnitVector(rng, 32));
  }

  const uint64_t allocations = HnswIndex::VisitedBufferAllocations();
  const auto start = std::chrono::steady_clock::now();
  size_t returned = 0;
  for (size_t i = 0; i < kSearches; ++i) {
    returned += index_->Search(queries[i % queries.size()].data(), 10).size();
  }
  const auto elapsed = std::chrono::steady_clock::now() - start;
  const double per_search_us = std::chrono::duration<double, std::micro>(elapsed).count() / kSearches;

  std::cout << "searches=" << kSearches << " per_search_us=" << per_search_us
            << " visited_buffer_allocations=" << (HnswIndex::VisitedBufferAllocations() - allocations) << "\n";
  EXPECT_GT(returned, 0U);
}

}  // namespace
}  // namespace nvecd::vectors
