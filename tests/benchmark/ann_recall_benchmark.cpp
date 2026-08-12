/**
 * @file ann_recall_benchmark.cpp
 * @brief Recall and latency measurement for the approximate indices
 *
 * A latency number for an approximate index means nothing on its own: any
 * ANN structure can be made arbitrarily fast by returning worse answers.
 * The figure that decides whether an index is usable is the trade-off
 * between the two, so this benchmark sweeps the knob that controls it
 * (ef_search for HNSW, nprobe for IVF) and reports recall next to latency
 * at every point on the curve.
 *
 * Ground truth is an exhaustive scan over the same vectors with the same
 * distance function, so recall here is measured against this engine's own
 * exact answer rather than against an external label set. That makes the
 * numbers reproducible anywhere the binary builds, with no dataset to fetch.
 *
 * All tests are DISABLED by default; run with
 * --gtest_also_run_disabled_tests to execute.
 *
 * This sweep is a measurement tool, not a gate. It builds a 50k-vector index
 * per dimension and corpus and computes exhaustive ground truth for every
 * query, which is far too slow to attach to a push and would be timed on a
 * shared runner even if it were. Run it by hand before changing the recall
 * curve published in the docs; the assertions below then check the run
 * against what is currently published rather than only printing a table.
 *
 * The cheap guard that does run on every push is
 * HnswIndexClusteredRecallTest.RecallOnClusteredDataDoesNotPlateau in
 * tests/vectors/hnsw_index_test.cpp — a few thousand vectors, under a second,
 * and enough to catch neighbour selection silently degrading the graph.
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <numeric>
#include <random>
#include <unordered_set>
#include <vector>

#include "vectors/distance.h"
#include "vectors/hnsw_index.h"
#include "vectors/ivf_index.h"

namespace nvecd::benchmark {
namespace {

/// Fixed seed so a re-run on any machine produces the same vectors.
constexpr uint32_t kSeed = 42;

/// Neighbours requested per query; recall is measured at this k.
constexpr uint32_t kTopK = 10;

/// Queries averaged per measurement point.
constexpr uint32_t kQueryCount = 200;

/// Dimensions to sweep: one typical of embeddings-lite, one of transformer output.
constexpr uint32_t kDimensions[] = {128, 768};

/// Corpus size per measurement.
constexpr uint32_t kVectorCount = 50000;

/// ef_search values for the HNSW curve.
constexpr uint32_t kEfSearchSweep[] = {10, 16, 32, 64, 128, 256, 512};

/// nprobe values for the IVF curve.
constexpr uint32_t kNprobeSweep[] = {1, 2, 4, 8, 16, 32, 64, 128};

/// Recall floors the documentation publishes for clustered data, checked so a
/// change that degrades graph or partition quality fails here instead of
/// silently making the published curve wrong. Uniform corpora are deliberately
/// left unchecked: near-orthogonal vectors are the pathological case for any
/// approximate index, and the documented conclusion there is that approximate
/// search does not pay, not that it hits a particular number.
struct RecallFloor {
  uint32_t knob;
  double min_recall;
};
constexpr RecallFloor kHnswFloors[] = {{10, 0.97}, {32, 0.99}, {64, 0.99}};
constexpr RecallFloor kIvfFloors[] = {{1, 0.90}, {4, 0.98}, {8, 0.98}};

/// Returns the floor for a knob value, or -1 when that value is not checked.
double FloorFor(const RecallFloor* floors, size_t count, uint32_t knob) {
  for (size_t i = 0; i < count; ++i) {
    if (floors[i].knob == knob)
      return floors[i].min_recall;
  }
  return -1.0;
}

/// Generate a unit-norm random vector.
std::vector<float> RandomUnitVector(std::mt19937& rng, uint32_t dim) {
  std::normal_distribution<float> dist(0.0F, 1.0F);
  std::vector<float> v(dim);
  float norm = 0.0F;
  for (uint32_t i = 0; i < dim; ++i) {
    v[i] = dist(rng);
    norm += v[i] * v[i];
  }
  norm = std::sqrt(norm);
  if (norm > 1e-7F) {
    for (uint32_t i = 0; i < dim; ++i) {
      v[i] /= norm;
    }
  }
  return v;
}

/// Corpus shapes worth measuring separately.
enum class Corpus : uint8_t {
  kUniform,   ///< Independent Gaussian directions: no structure to exploit.
  kClustered  ///< Points drawn around centroids: the shape real embeddings have.
};

/// Number of latent clusters in the clustered corpus.
constexpr uint32_t kClusterCount = 200;

/// Spread of each cluster, as a fraction of the centroid's unit norm.
///
/// Applied per dimension the noise would grow as sqrt(dim) and swamp the
/// centroid, turning the "clustered" corpus back into a uniform one. The
/// per-dimension sigma is therefore divided by sqrt(dim) so the perturbation
/// keeps this magnitude relative to the centroid at any dimension.
constexpr float kClusterSpread = 0.25F;

float PerDimensionSigma(uint32_t dim, float spread) {
  return spread / std::sqrt(static_cast<float>(dim));
}

const char* CorpusName(Corpus corpus) {
  return corpus == Corpus::kUniform ? "uniform" : "clustered";
}

/// Build a [count x dim] matrix of unit vectors.
///
/// Uniformly random directions are near-orthogonal in high dimensions, which
/// leaves neighbours separated by almost nothing and is the hardest case any
/// graph or partition index can be given. Real embeddings are not shaped that
/// way, so both corpora are measured: the uniform one bounds the worst case,
/// the clustered one shows what to expect from data with structure.
std::vector<float> BuildCorpus(uint32_t count, uint32_t dim, std::mt19937& rng, Corpus corpus) {
  std::vector<float> matrix(static_cast<size_t>(count) * dim);
  if (corpus == Corpus::kUniform) {
    for (uint32_t i = 0; i < count; ++i) {
      std::vector<float> vec = RandomUnitVector(rng, dim);
      std::copy(vec.begin(), vec.end(), matrix.begin() + static_cast<size_t>(i) * dim);
    }
    return matrix;
  }

  std::vector<std::vector<float>> centroids;
  centroids.reserve(kClusterCount);
  for (uint32_t c = 0; c < kClusterCount; ++c) {
    centroids.push_back(RandomUnitVector(rng, dim));
  }

  std::normal_distribution<float> jitter(0.0F, PerDimensionSigma(dim, kClusterSpread));
  std::uniform_int_distribution<uint32_t> pick(0, kClusterCount - 1);
  for (uint32_t i = 0; i < count; ++i) {
    const std::vector<float>& centroid = centroids[pick(rng)];
    std::vector<float> vec(dim);
    float norm = 0.0F;
    for (uint32_t d = 0; d < dim; ++d) {
      vec[d] = centroid[d] + jitter(rng);
      norm += vec[d] * vec[d];
    }
    norm = std::sqrt(norm);
    if (norm > 1e-7F) {
      for (uint32_t d = 0; d < dim; ++d) {
        vec[d] /= norm;
      }
    }
    std::copy(vec.begin(), vec.end(), matrix.begin() + static_cast<size_t>(i) * dim);
  }
  return matrix;
}

/// Draw a query from the same distribution as the corpus it will search.
std::vector<float> BuildQuery(uint32_t dim, std::mt19937& rng, Corpus corpus, const std::vector<float>& matrix,
                              uint32_t count) {
  if (corpus == Corpus::kUniform) {
    return RandomUnitVector(rng, dim);
  }
  // Perturb an existing point so the query lands inside the data's structure,
  // which is how a recommendation query actually arrives.
  std::uniform_int_distribution<uint32_t> pick(0, count - 1);
  const float* base = matrix.data() + static_cast<size_t>(pick(rng)) * dim;
  std::normal_distribution<float> jitter(0.0F, PerDimensionSigma(dim, kClusterSpread / 2.0F));
  std::vector<float> vec(dim);
  float norm = 0.0F;
  for (uint32_t d = 0; d < dim; ++d) {
    vec[d] = base[d] + jitter(rng);
    norm += vec[d] * vec[d];
  }
  norm = std::sqrt(norm);
  if (norm > 1e-7F) {
    for (uint32_t d = 0; d < dim; ++d) {
      vec[d] /= norm;
    }
  }
  return vec;
}

/// Exhaustive top-k over the corpus — the exact answer recall is scored against.
std::vector<uint32_t> ExactTopK(const float* query, const float* matrix, uint32_t count, uint32_t dim, uint32_t top_k) {
  std::vector<std::pair<float, uint32_t>> scored(count);
  for (uint32_t i = 0; i < count; ++i) {
    scored[i] = {vectors::CosineDistanceRaw(query, matrix + static_cast<size_t>(i) * dim, dim), i};
  }
  const uint32_t k = std::min(top_k, count);
  std::partial_sort(scored.begin(), scored.begin() + k, scored.end(),
                    [](const auto& lhs, const auto& rhs) { return lhs.first > rhs.first; });
  std::vector<uint32_t> ids(k);
  for (uint32_t i = 0; i < k; ++i) {
    ids[i] = scored[i].second;
  }
  return ids;
}

/// Fraction of the exact top-k that the approximate result also returned.
double RecallAt(const std::vector<uint32_t>& approx, const std::vector<uint32_t>& exact) {
  if (exact.empty()) {
    return 1.0;
  }
  const std::unordered_set<uint32_t> truth(exact.begin(), exact.end());
  uint32_t hits = 0;
  for (uint32_t id : approx) {
    if (truth.count(id) != 0U) {
      ++hits;
    }
  }
  return static_cast<double>(hits) / static_cast<double>(exact.size());
}

double Percentile(std::vector<double> samples, double q) {
  if (samples.empty()) {
    return 0.0;
  }
  std::sort(samples.begin(), samples.end());
  const size_t rank = std::min(samples.size(), static_cast<size_t>(std::ceil(q * samples.size())));
  return samples[rank == 0 ? 0 : rank - 1];
}

void PrintCurveHeader(const char* knob) {
  std::cout << "\n  " << std::left << std::setw(10) << knob << std::right << std::setw(12) << "recall@10"
            << std::setw(14) << "p50 (us)" << std::setw(14) << "p99 (us)" << std::setw(16) << "vs exact scan"
            << "\n  " << std::string(10, '-') << std::string(12, '-') << std::string(14, '-') << std::string(14, '-')
            << std::string(16, '-') << "\n";
}

void PrintCurveRow(uint32_t knob, double recall, double p50, double p99, double exact_p50) {
  std::cout << "  " << std::left << std::setw(10) << knob << std::right << std::setw(11) << std::fixed
            << std::setprecision(4) << recall << " " << std::setw(13) << std::setprecision(1) << p50 << std::setw(14)
            << p99 << std::setw(15) << std::setprecision(1) << (p50 > 0.0 ? exact_p50 / p50 : 0.0) << "x"
            << "\n";
}

}  // namespace

// ---------------------------------------------------------------------------
// HNSW: recall vs ef_search
// ---------------------------------------------------------------------------

TEST(AnnRecallBenchmark, DISABLED_HnswRecallVsEfSearch) {
  for (Corpus corpus : {Corpus::kUniform, Corpus::kClustered}) {
    for (uint32_t dim : kDimensions) {
      std::mt19937 rng(kSeed);
      const std::vector<float> matrix = BuildCorpus(kVectorCount, dim, rng, corpus);

      std::vector<std::vector<float>> queries;
      queries.reserve(kQueryCount);
      for (uint32_t q = 0; q < kQueryCount; ++q) {
        queries.push_back(BuildQuery(dim, rng, corpus, matrix, kVectorCount));
      }

      // Exact answers, and the cost of producing them.
      std::vector<std::vector<uint32_t>> truth;
      truth.reserve(kQueryCount);
      std::vector<double> exact_times;
      exact_times.reserve(kQueryCount);
      for (const auto& query : queries) {
        const auto start = std::chrono::steady_clock::now();
        truth.push_back(ExactTopK(query.data(), matrix.data(), kVectorCount, dim, kTopK));
        const auto end = std::chrono::steady_clock::now();
        exact_times.push_back(std::chrono::duration<double, std::micro>(end - start).count());
      }
      const double exact_p50 = Percentile(exact_times, 0.50);

      vectors::HnswIndex::Config config;
      config.m = 16;
      config.ef_construction = 200;
      config.max_elements = kVectorCount;
      vectors::HnswIndex index(dim, vectors::CosineDistanceRaw, config);

      const auto build_start = std::chrono::steady_clock::now();
      for (uint32_t i = 0; i < kVectorCount; ++i) {
        index.Add(i, matrix.data() + static_cast<size_t>(i) * dim);
      }
      const auto build_end = std::chrono::steady_clock::now();
      const double build_ms = std::chrono::duration<double, std::milli>(build_end - build_start).count();

      std::cout << "\n=== HNSW  corpus=" << CorpusName(corpus) << "  dim=" << dim << "  vectors=" << kVectorCount
                << "  M=" << config.m << "  ef_construction=" << config.ef_construction << "  top_k=" << kTopK
                << "  queries=" << kQueryCount << "\n    build: " << std::fixed << std::setprecision(1) << build_ms
                << " ms"
                << "   memory: " << (index.MemoryUsage() / (1024 * 1024)) << " MB"
                << "   exact scan p50: " << std::setprecision(1) << exact_p50 << " us\n";
      PrintCurveHeader("ef_search");

      for (uint32_t ef : kEfSearchSweep) {
        index.SetEfSearch(ef);
        double recall_sum = 0.0;
        std::vector<double> times;
        times.reserve(kQueryCount);
        for (uint32_t q = 0; q < kQueryCount; ++q) {
          const auto start = std::chrono::steady_clock::now();
          const auto results = index.Search(queries[q].data(), kTopK);
          const auto end = std::chrono::steady_clock::now();
          times.push_back(std::chrono::duration<double, std::micro>(end - start).count());

          std::vector<uint32_t> ids;
          ids.reserve(results.size());
          for (const auto& [id, score] : results) {
            ids.push_back(id);
          }
          recall_sum += RecallAt(ids, truth[q]);
        }
        const double recall = recall_sum / kQueryCount;
        PrintCurveRow(ef, recall, Percentile(times, 0.50), Percentile(times, 0.99), exact_p50);

        if (corpus == Corpus::kClustered) {
          const double floor = FloorFor(kHnswFloors, std::size(kHnswFloors), ef);
          if (floor >= 0.0) {
            EXPECT_GE(recall, floor) << "HNSW recall@" << kTopK << " at ef_search=" << ef << " (dim " << dim
                                     << ") fell below the published curve";
          }
        }
      }
    }
  }
}

// ---------------------------------------------------------------------------
// IVF: recall vs nprobe
// ---------------------------------------------------------------------------

TEST(AnnRecallBenchmark, DISABLED_IvfRecallVsNprobe) {
  for (Corpus corpus : {Corpus::kUniform, Corpus::kClustered}) {
    for (uint32_t dim : kDimensions) {
      std::mt19937 rng(kSeed);
      const std::vector<float> matrix = BuildCorpus(kVectorCount, dim, rng, corpus);

      std::vector<float> norms(kVectorCount);
      for (uint32_t i = 0; i < kVectorCount; ++i) {
        const float* v = matrix.data() + static_cast<size_t>(i) * dim;
        float sum = 0.0F;
        for (uint32_t d = 0; d < dim; ++d) {
          sum += v[d] * v[d];
        }
        norms[i] = std::sqrt(sum);
      }

      std::vector<std::vector<float>> queries;
      queries.reserve(kQueryCount);
      for (uint32_t q = 0; q < kQueryCount; ++q) {
        queries.push_back(BuildQuery(dim, rng, corpus, matrix, kVectorCount));
      }

      std::vector<std::vector<uint32_t>> truth;
      truth.reserve(kQueryCount);
      std::vector<double> exact_times;
      exact_times.reserve(kQueryCount);
      for (const auto& query : queries) {
        const auto start = std::chrono::steady_clock::now();
        truth.push_back(ExactTopK(query.data(), matrix.data(), kVectorCount, dim, kTopK));
        const auto end = std::chrono::steady_clock::now();
        exact_times.push_back(std::chrono::duration<double, std::micro>(end - start).count());
      }
      const double exact_p50 = Percentile(exact_times, 0.50);

      vectors::IvfIndex::Config config;
      config.nlist = 256;
      config.train_threshold = 1000;
      vectors::IvfIndex index(dim, config);

      std::vector<size_t> valid(kVectorCount);
      std::iota(valid.begin(), valid.end(), 0U);

      const auto build_start = std::chrono::steady_clock::now();
      index.Train(matrix.data(), valid.data(), valid.size(), dim, true);
      const auto build_end = std::chrono::steady_clock::now();
      const double build_ms = std::chrono::duration<double, std::milli>(build_end - build_start).count();

      std::cout << "\n=== IVF  corpus=" << CorpusName(corpus) << "  dim=" << dim << "  vectors=" << kVectorCount
                << "  nlist=" << config.nlist << "  top_k=" << kTopK << "  queries=" << kQueryCount
                << "\n    train: " << std::fixed << std::setprecision(1) << build_ms << " ms"
                << "   clusters: " << index.GetClusterCount() << "   indexed: " << index.GetIndexedCount()
                << "   exact scan p50: " << std::setprecision(1) << exact_p50 << " us\n";
      PrintCurveHeader("nprobe");

      for (uint32_t nprobe : kNprobeSweep) {
        if (nprobe > config.nlist) {
          continue;
        }
        index.SetNprobe(nprobe);
        double recall_sum = 0.0;
        std::vector<double> times;
        times.reserve(kQueryCount);
        for (uint32_t q = 0; q < kQueryCount; ++q) {
          const auto start = std::chrono::steady_clock::now();
          const auto results =
              index.Search(queries[q].data(), 1.0F, matrix.data(), norms.data(), kVectorCount, dim, kTopK);
          const auto end = std::chrono::steady_clock::now();
          times.push_back(std::chrono::duration<double, std::micro>(end - start).count());

          std::vector<uint32_t> ids;
          ids.reserve(results.size());
          for (const auto& [score, id] : results) {
            ids.push_back(static_cast<uint32_t>(id));
          }
          recall_sum += RecallAt(ids, truth[q]);
        }
        const double recall = recall_sum / kQueryCount;
        PrintCurveRow(nprobe, recall, Percentile(times, 0.50), Percentile(times, 0.99), exact_p50);

        if (corpus == Corpus::kClustered) {
          const double floor = FloorFor(kIvfFloors, std::size(kIvfFloors), nprobe);
          if (floor >= 0.0) {
            EXPECT_GE(recall, floor) << "IVF recall@" << kTopK << " at nprobe=" << nprobe << " (dim " << dim
                                     << ") fell below the published curve";
          }
        }
      }
    }
  }
}

}  // namespace nvecd::benchmark
