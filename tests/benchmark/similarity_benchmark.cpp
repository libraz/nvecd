/**
 * @file similarity_benchmark.cpp
 * @brief Benchmark for similarity search pipeline components
 *
 * Profiles each stage of the similarity search pipeline directly
 * (no TCP overhead). All tests are DISABLED by default; run with
 * --gtest_also_run_disabled_tests to execute.
 *
 * Measures:
 * 1. VectorStore::Compact() rebuild time
 * 2. Brute-force scan with CosineSimilarityPreNorm
 * 3. MergeAndSelectTopK (partial_sort overhead)
 * 4. Reservoir sampling (SampleIndices overhead)
 * 5. End-to-end SearchByIdVectors
 * 6. End-to-end SearchByVector (SIMV)
 * 7. Cache hit vs miss latency
 */

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <random>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "cache/cache_key.h"
#include "cache/similarity_cache.h"
#include "config/config.h"
#include "events/co_occurrence_index.h"
#include "events/event_store.h"
#include "similarity/similarity_engine.h"
#include "vectors/distance.h"
#include "vectors/vector_store.h"

namespace nvecd::benchmark {
namespace {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/// Fixed seed for reproducibility.
constexpr uint32_t kSeed = 42;

/// Benchmark dimension.
constexpr size_t kDim = 128;

/// Default top-k for queries.
constexpr int kTopK = 10;

/// Number of iterations per measurement.
constexpr int kIterations = 15;

/// Scales to measure.
constexpr size_t kScales[] = {1000, 10000, 50000, 100000};
constexpr size_t kNumScales = sizeof(kScales) / sizeof(kScales[0]);

/// Generate a random vector of given dimension.
std::vector<float> RandomVector(std::mt19937& rng, size_t dim) {
  std::normal_distribution<float> dist(0.0F, 1.0F);
  std::vector<float> v(dim);
  for (size_t i = 0; i < dim; ++i) {
    v[i] = dist(rng);
  }
  return v;
}

/// Return the median of a vector of durations (modifies input).
double MedianUs(std::vector<double>& samples) {
  std::sort(samples.begin(), samples.end());
  size_t n = samples.size();
  if (n % 2 == 0) {
    return (samples[n / 2 - 1] + samples[n / 2]) / 2.0;
  }
  return samples[n / 2];
}

/// Print a table header.
void PrintHeader(const std::string& title) {
  std::cout << "\n"
            << std::string(78, '=') << "\n"
            << "  " << title << "\n"
            << std::string(78, '=') << "\n"
            << std::left << std::setw(12) << "Scale" << std::setw(18) << "Median (us)"
            << std::setw(18) << "Median (ms)" << std::setw(18) << "Ops/sec"
            << "\n"
            << std::string(78, '-') << "\n";
}

/// Print a table row.
void PrintRow(size_t scale, double median_us) {
  double median_ms = median_us / 1000.0;
  double ops_sec = (median_us > 0.0) ? 1'000'000.0 / median_us : 0.0;
  std::cout << std::left << std::setw(12) << scale << std::fixed << std::setprecision(1) << std::setw(18) << median_us
            << std::setw(18) << median_ms << std::setprecision(0) << std::setw(18) << ops_sec << "\n";
}

/// Populate a VectorStore with `count` random vectors of dimension `dim`.
void PopulateStore(vectors::VectorStore& store, size_t count, size_t dim, std::mt19937& rng) {
  for (size_t i = 0; i < count; ++i) {
    auto vec = RandomVector(rng, dim);
    auto id = "item_" + std::to_string(i);
    auto result = store.SetVector(id, vec);
    if (!result) {
      FAIL() << "SetVector failed for " << id << ": " << result.error().message();
    }
  }
}

// ---------------------------------------------------------------------------
// Benchmark fixture
// ---------------------------------------------------------------------------

class SimilarityBenchmark : public ::testing::Test {
 protected:
  void SetUp() override {}
  void TearDown() override {}
};

// ---------------------------------------------------------------------------
// 1. Compact() time
// ---------------------------------------------------------------------------

TEST_F(SimilarityBenchmark, DISABLED_CompactTime) {
  PrintHeader("VectorStore::Compact() rebuild time");

  for (size_t si = 0; si < kNumScales; ++si) {
    size_t scale = kScales[si];
    config::VectorsConfig vcfg;
    vcfg.default_dimension = kDim;
    vectors::VectorStore store(vcfg);

    std::mt19937 rng(kSeed);
    PopulateStore(store, scale, kDim, rng);

    std::vector<double> samples;
    samples.reserve(kIterations);

    for (int iter = 0; iter < kIterations; ++iter) {
      auto start = std::chrono::high_resolution_clock::now();
      store.Compact();
      auto end = std::chrono::high_resolution_clock::now();
      double us = std::chrono::duration<double, std::micro>(end - start).count();
      samples.push_back(us);
    }

    PrintRow(scale, MedianUs(samples));
  }

  std::cout << std::string(78, '-') << "\n";
}

// ---------------------------------------------------------------------------
// 2. Brute-force scan with CosineSimilarityPreNorm
// ---------------------------------------------------------------------------

TEST_F(SimilarityBenchmark, DISABLED_BruteForceScan) {
  PrintHeader("Brute-force CosineSimilarityPreNorm scan (top_k=" + std::to_string(kTopK) + ")");

  for (size_t si = 0; si < kNumScales; ++si) {
    size_t scale = kScales[si];
    config::VectorsConfig vcfg;
    vcfg.default_dimension = kDim;
    vectors::VectorStore store(vcfg);

    std::mt19937 rng(kSeed);
    PopulateStore(store, scale, kDim, rng);
    store.Compact();

    // Generate a query vector and pre-compute its norm.
    auto query = RandomVector(rng, kDim);
    float query_norm = vectors::L2Norm(query);

    std::vector<double> samples;
    samples.reserve(kIterations);

    for (int iter = 0; iter < kIterations; ++iter) {
      auto lock = store.AcquireReadLock();
      size_t n = store.GetCompactCount();

      auto start = std::chrono::high_resolution_clock::now();

      // Full scan computing cosine similarity with pre-computed norms.
      std::vector<std::pair<size_t, float>> scores;
      scores.reserve(n);
      for (size_t i = 0; i < n; ++i) {
        float sim = vectors::CosineSimilarityPreNorm(query.data(), store.GetMatrixRow(i), kDim, query_norm,
                                                     store.GetNorm(i));
        scores.emplace_back(i, sim);
      }

      // partial_sort to get top-k.
      size_t k = std::min(static_cast<size_t>(kTopK), n);
      std::partial_sort(scores.begin(), scores.begin() + static_cast<ptrdiff_t>(k), scores.end(),
                        [](const auto& a, const auto& b) { return a.second > b.second; });

      auto end = std::chrono::high_resolution_clock::now();
      double us = std::chrono::duration<double, std::micro>(end - start).count();
      samples.push_back(us);

      // Prevent optimizer from eliding the work.
      ASSERT_GE(scores.size(), k);
    }

    PrintRow(scale, MedianUs(samples));
  }

  std::cout << std::string(78, '-') << "\n";
}

// ---------------------------------------------------------------------------
// 3. MergeAndSelectTopK (partial_sort overhead)
// ---------------------------------------------------------------------------

TEST_F(SimilarityBenchmark, DISABLED_MergeAndSelectTopK) {
  PrintHeader("MergeAndSelectTopK partial_sort overhead (top_k=" + std::to_string(kTopK) + ")");

  for (size_t si = 0; si < kNumScales; ++si) {
    size_t scale = kScales[si];
    std::mt19937 rng(kSeed);

    // Build a vector of SimilarityResult to sort.
    std::vector<similarity::SimilarityResult> results;
    results.reserve(scale);
    std::uniform_real_distribution<float> score_dist(0.0F, 1.0F);
    for (size_t i = 0; i < scale; ++i) {
      results.emplace_back("item_" + std::to_string(i), score_dist(rng));
    }

    std::vector<double> samples;
    samples.reserve(kIterations);

    for (int iter = 0; iter < kIterations; ++iter) {
      // Copy because partial_sort is destructive.
      auto copy = results;

      auto start = std::chrono::high_resolution_clock::now();

      size_t k = std::min(static_cast<size_t>(kTopK), copy.size());
      std::partial_sort(copy.begin(), copy.begin() + static_cast<ptrdiff_t>(k), copy.end());
      copy.resize(k);

      auto end = std::chrono::high_resolution_clock::now();
      double us = std::chrono::duration<double, std::micro>(end - start).count();
      samples.push_back(us);

      ASSERT_EQ(copy.size(), k);
    }

    PrintRow(scale, MedianUs(samples));
  }

  std::cout << std::string(78, '-') << "\n";
}

// ---------------------------------------------------------------------------
// 4. Reservoir sampling (SampleIndices overhead)
// ---------------------------------------------------------------------------

TEST_F(SimilarityBenchmark, DISABLED_ReservoirSampling) {
  PrintHeader("Reservoir sampling (sample_size=5000)");

  constexpr size_t kSampleSize = 5000;

  for (size_t si = 0; si < kNumScales; ++si) {
    size_t scale = kScales[si];
    if (scale < kSampleSize) {
      std::cout << std::left << std::setw(12) << scale << "  (skipped, scale < sample_size)\n";
      continue;
    }

    std::vector<double> samples;
    samples.reserve(kIterations);

    for (int iter = 0; iter < kIterations; ++iter) {
      std::mt19937 rng(kSeed + static_cast<uint32_t>(iter));

      auto start = std::chrono::high_resolution_clock::now();

      // Reservoir sampling: select kSampleSize indices from [0, scale).
      std::vector<size_t> reservoir(kSampleSize);
      std::iota(reservoir.begin(), reservoir.end(), 0);
      for (size_t i = kSampleSize; i < scale; ++i) {
        std::uniform_int_distribution<size_t> dist(0, i);
        size_t j = dist(rng);
        if (j < kSampleSize) {
          reservoir[j] = i;
        }
      }

      auto end = std::chrono::high_resolution_clock::now();
      double us = std::chrono::duration<double, std::micro>(end - start).count();
      samples.push_back(us);

      ASSERT_EQ(reservoir.size(), kSampleSize);
    }

    PrintRow(scale, MedianUs(samples));
  }

  std::cout << std::string(78, '-') << "\n";
}

// ---------------------------------------------------------------------------
// 5. End-to-end SearchByIdVectors
// ---------------------------------------------------------------------------

TEST_F(SimilarityBenchmark, DISABLED_SearchByIdVectors) {
  PrintHeader("End-to-end SearchByIdVectors (top_k=" + std::to_string(kTopK) + ")");

  for (size_t si = 0; si < kNumScales; ++si) {
    size_t scale = kScales[si];
    config::VectorsConfig vcfg;
    vcfg.default_dimension = kDim;
    vcfg.distance_metric = "cosine";
    vectors::VectorStore store(vcfg);

    std::mt19937 rng(kSeed);
    PopulateStore(store, scale, kDim, rng);
    store.Compact();

    config::EventsConfig ecfg;
    events::EventStore event_store(ecfg);
    events::CoOccurrenceIndex co_index;

    config::SimilarityConfig scfg;
    similarity::SimilarityEngine engine(&event_store, &co_index, &store, scfg, vcfg);

    // Pick a query ID that exists.
    std::string query_id = "item_0";

    std::vector<double> samples;
    samples.reserve(kIterations);

    for (int iter = 0; iter < kIterations; ++iter) {
      auto start = std::chrono::high_resolution_clock::now();
      auto result = engine.SearchByIdVectors(query_id, kTopK);
      auto end = std::chrono::high_resolution_clock::now();

      ASSERT_TRUE(result.has_value()) << result.error().message();
      ASSERT_LE(result->size(), static_cast<size_t>(kTopK));

      double us = std::chrono::duration<double, std::micro>(end - start).count();
      samples.push_back(us);
    }

    PrintRow(scale, MedianUs(samples));
  }

  std::cout << std::string(78, '-') << "\n";
}

// ---------------------------------------------------------------------------
// 6. End-to-end SearchByVector (SIMV)
// ---------------------------------------------------------------------------

TEST_F(SimilarityBenchmark, DISABLED_SearchByVector) {
  PrintHeader("End-to-end SearchByVector / SIMV (top_k=" + std::to_string(kTopK) + ")");

  for (size_t si = 0; si < kNumScales; ++si) {
    size_t scale = kScales[si];
    config::VectorsConfig vcfg;
    vcfg.default_dimension = kDim;
    vcfg.distance_metric = "cosine";
    vectors::VectorStore store(vcfg);

    std::mt19937 rng(kSeed);
    PopulateStore(store, scale, kDim, rng);
    store.Compact();

    config::EventsConfig ecfg;
    events::EventStore event_store(ecfg);
    events::CoOccurrenceIndex co_index;

    config::SimilarityConfig scfg;
    similarity::SimilarityEngine engine(&event_store, &co_index, &store, scfg, vcfg);

    // Generate an external query vector.
    auto query = RandomVector(rng, kDim);

    std::vector<double> samples;
    samples.reserve(kIterations);

    for (int iter = 0; iter < kIterations; ++iter) {
      auto start = std::chrono::high_resolution_clock::now();
      auto result = engine.SearchByVector(query, kTopK);
      auto end = std::chrono::high_resolution_clock::now();

      ASSERT_TRUE(result.has_value()) << result.error().message();
      ASSERT_LE(result->size(), static_cast<size_t>(kTopK));

      double us = std::chrono::duration<double, std::micro>(end - start).count();
      samples.push_back(us);
    }

    PrintRow(scale, MedianUs(samples));
  }

  std::cout << std::string(78, '-') << "\n";
}

// ---------------------------------------------------------------------------
// 7. Cache hit vs miss latency
// ---------------------------------------------------------------------------

TEST_F(SimilarityBenchmark, DISABLED_CacheHitVsMiss) {
  PrintHeader("SimilarityCache lookup latency (hit vs miss)");

  constexpr size_t kCacheMemory = 64 * 1024 * 1024;  // 64 MB
  constexpr double kMinQueryCost = 0.0;               // Cache everything
  constexpr int kTtlSeconds = 0;                      // No TTL

  // Build some results to cache.
  std::vector<similarity::SimilarityResult> dummy_results;
  dummy_results.reserve(kTopK);
  for (int i = 0; i < kTopK; ++i) {
    dummy_results.emplace_back("item_" + std::to_string(i), 1.0F - static_cast<float>(i) * 0.05F);
  }

  // Measure at different numbers of cached entries to test LRU overhead.
  constexpr size_t kEntryCounts[] = {100, 1000, 10000, 50000};

  std::cout << "\n--- Cache Miss (Lookup of non-existent key) ---\n";
  std::cout << std::left << std::setw(12) << "Entries" << std::setw(18) << "Median (us)" << std::setw(18)
            << "Median (ns)" << std::setw(18) << "Ops/sec"
            << "\n"
            << std::string(66, '-') << "\n";

  for (size_t entry_count : kEntryCounts) {
    cache::SimilarityCache cache(kCacheMemory, kMinQueryCost, kTtlSeconds);

    // Insert entries.
    for (size_t i = 0; i < entry_count; ++i) {
      auto key = cache::CacheKeyGenerator::Generate("SIM item_" + std::to_string(i) + " 10");
      cache.Insert(key, dummy_results, 50.0);
    }

    // Lookup a key that does not exist.
    auto miss_key = cache::CacheKeyGenerator::Generate("SIM nonexistent_item 10");

    std::vector<double> samples;
    samples.reserve(kIterations);

    for (int iter = 0; iter < kIterations; ++iter) {
      auto start = std::chrono::high_resolution_clock::now();
      auto result = cache.Lookup(miss_key);
      auto end = std::chrono::high_resolution_clock::now();

      ASSERT_FALSE(result.has_value());
      double us = std::chrono::duration<double, std::micro>(end - start).count();
      samples.push_back(us);
    }

    double median = MedianUs(samples);
    double median_ns = median * 1000.0;
    double ops_sec = (median > 0.0) ? 1'000'000.0 / median : 0.0;
    std::cout << std::left << std::setw(12) << entry_count << std::fixed << std::setprecision(3) << std::setw(18)
              << median << std::setprecision(0) << std::setw(18) << median_ns << std::setw(18) << ops_sec << "\n";
  }

  std::cout << "\n--- Cache Hit (Lookup of existing key) ---\n";
  std::cout << std::left << std::setw(12) << "Entries" << std::setw(18) << "Median (us)" << std::setw(18)
            << "Median (ns)" << std::setw(18) << "Ops/sec"
            << "\n"
            << std::string(66, '-') << "\n";

  for (size_t entry_count : kEntryCounts) {
    cache::SimilarityCache cache(kCacheMemory, kMinQueryCost, kTtlSeconds);

    // Insert entries.
    for (size_t i = 0; i < entry_count; ++i) {
      auto key = cache::CacheKeyGenerator::Generate("SIM item_" + std::to_string(i) + " 10");
      cache.Insert(key, dummy_results, 50.0);
    }

    // Lookup a key that exists (use last inserted to avoid LRU eviction).
    auto hit_key = cache::CacheKeyGenerator::Generate("SIM item_" + std::to_string(entry_count - 1) + " 10");

    std::vector<double> samples;
    samples.reserve(kIterations);

    for (int iter = 0; iter < kIterations; ++iter) {
      auto start = std::chrono::high_resolution_clock::now();
      auto result = cache.Lookup(hit_key);
      auto end = std::chrono::high_resolution_clock::now();

      ASSERT_TRUE(result.has_value());
      double us = std::chrono::duration<double, std::micro>(end - start).count();
      samples.push_back(us);
    }

    double median = MedianUs(samples);
    double median_ns = median * 1000.0;
    double ops_sec = (median > 0.0) ? 1'000'000.0 / median : 0.0;
    std::cout << std::left << std::setw(12) << entry_count << std::fixed << std::setprecision(3) << std::setw(18)
              << median << std::setprecision(0) << std::setw(18) << median_ns << std::setw(18) << ops_sec << "\n";
  }

  std::cout << std::string(66, '-') << "\n";
}

}  // namespace
}  // namespace nvecd::benchmark
