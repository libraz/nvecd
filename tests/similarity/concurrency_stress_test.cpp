/**
 * @file concurrency_stress_test.cpp
 * @brief Concurrency stress tests for VectorStore + SimilarityEngine
 *
 * Exercises the lock-owning CompactSnapshot path: many writer threads
 * mutate overlapping IDs (SetVector/DeleteVector, forcing matrix
 * reallocation and defragmentation) while many reader threads run
 * similarity searches. The test must complete without crashing
 * (heap-use-after-free / torn reads) and with self-consistent results.
 *
 * Run under ThreadSanitizer (cmake -DENABLE_TSAN=ON) to verify the
 * absence of data races on the compact storage.
 */

#include <gtest/gtest.h>

#include <atomic>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "similarity/similarity_engine.h"
#include "vectors/vector_store.h"

namespace nvecd::similarity {
namespace {

constexpr uint32_t kDim = 8;
constexpr size_t kIdSpace = 200;  ///< Overlapping ID range shared by all threads

config::VectorsConfig MakeVectorsConfig() {
  config::VectorsConfig config;
  config.default_dimension = kDim;
  config.distance_metric = "cosine";
  return config;
}

config::SimilarityConfig MakeSimilarityConfig(const std::string& index_type) {
  config::SimilarityConfig config;
  config.default_top_k = 10;
  config.max_top_k = 1000;
  config.fusion_alpha = 0.6;
  config.fusion_beta = 0.4;
  config.sample_size = 0;  // exact scan for flat
  config.index_type = index_type;
  // Keep IVF thresholds low so training actually triggers during the test.
  config.ivf_train_threshold = 32;
  config.ivf_seal_threshold = 64;
  config.ivf_nlist = 8;
  config.ivf_nprobe = 4;
  return config;
}

std::vector<float> MakeVector(std::mt19937& rng, uint32_t dim) {
  std::uniform_real_distribution<float> dist(-1.0F, 1.0F);
  std::vector<float> vec(dim);
  for (auto& component : vec) {
    component = dist(rng);
  }
  return vec;
}

/// True for an id the workload could have written: results must never name a row
/// the store was never given, whatever stale mapping a search resolved through.
bool IsKnownId(const std::string& id) {
  constexpr std::string_view kPrefix = "item";
  if (id.compare(0, kPrefix.size(), kPrefix) != 0 || id.size() == kPrefix.size()) {
    return false;
  }
  size_t number = 0;
  for (size_t i = kPrefix.size(); i < id.size(); ++i) {
    if (id[i] < '0' || id[i] > '9') {
      return false;
    }
    number = number * 10 + static_cast<size_t>(id[i] - '0');
  }
  return number < kIdSpace;
}

/// Writes per writer thread for the index types whose publish path is cheap.
constexpr int kDefaultIterations = 400;

// Run a stress workload with the given index type and assert no crash.
void RunStress(const std::string& index_type, bool exercise_dispatcher_path, int iterations = kDefaultIterations) {
  auto vectors_config = MakeVectorsConfig();
  auto similarity_config = MakeSimilarityConfig(index_type);

  events::EventStore event_store(config::EventsConfig{});
  events::CoOccurrenceIndex co_index;
  vectors::VectorStore vector_store(vectors_config);
  SimilarityEngine engine(&event_store, &co_index, &vector_store, similarity_config, vectors_config);

  // Seed the store so readers have data immediately.
  {
    std::mt19937 rng(1);
    for (size_t i = 0; i < kIdSpace; ++i) {
      auto vec = MakeVector(rng, kDim);
      ASSERT_TRUE(vector_store.SetVector("item" + std::to_string(i), vec).has_value());
      // Mirror the production write path so IVF training/sealing is exercised.
      if (exercise_dispatcher_path) {
        auto idx = vector_store.GetCompactIndex("item" + std::to_string(i));
        if (idx.has_value()) {
          engine.NotifyVectorAdded(idx.value(), vec.data());
        }
      }
    }
  }

  constexpr int kWriters = 4;
  constexpr int kReaders = 4;
  std::atomic<bool> stop{false};
  std::atomic<size_t> read_ops{0};

  std::vector<std::thread> threads;

  // Writer threads: set/delete overlapping IDs to force reallocation/defrag.
  for (int w = 0; w < kWriters; ++w) {
    threads.emplace_back([&, w]() {
      std::mt19937 rng(static_cast<uint32_t>(100 + w));
      std::uniform_int_distribution<size_t> id_dist(0, kIdSpace - 1);
      for (int iter = 0; iter < iterations; ++iter) {
        size_t id_num = id_dist(rng);
        std::string id = "item" + std::to_string(id_num);
        if ((iter & 3) == 0) {
          if (exercise_dispatcher_path) {
            // The production delete sequence: a compaction re-keys every row and
            // rebuilds the index, a tombstone retires the one entry.
            const auto compact_index = vector_store.GetCompactIndex(id);
            const size_t rows_before = vector_store.GetCompactCount();
            if (compact_index.has_value() && vector_store.DeleteVector(id)) {
              if (vector_store.GetCompactCount() < rows_before) {
                engine.RebuildAnnFromStore();
              } else {
                engine.NotifyVectorRemoved(*compact_index);
              }
            }
          } else {
            vector_store.DeleteVector(id);
          }
        } else {
          auto vec = MakeVector(rng, kDim);
          if (vector_store.SetVector(id, vec).has_value() && exercise_dispatcher_path) {
            auto idx = vector_store.GetCompactIndex(id);
            if (idx.has_value()) {
              engine.NotifyVectorAdded(idx.value(), vec.data());
            }
          }
        }
      }
    });
  }

  // Reader threads: run similarity searches concurrently with the writers.
  for (int r = 0; r < kReaders; ++r) {
    threads.emplace_back([&, r]() {
      std::mt19937 rng(static_cast<uint32_t>(500 + r));
      std::uniform_int_distribution<size_t> id_dist(0, kIdSpace - 1);
      while (!stop.load(std::memory_order_relaxed)) {
        // SearchByVector (arbitrary query vector)
        auto query = MakeVector(rng, kDim);
        auto by_vec = engine.SearchByVector(query, 10, {});
        if (by_vec.has_value()) {
          // Self-consistency: results sorted by score descending.
          for (size_t i = 1; i < by_vec->size(); ++i) {
            EXPECT_GE((*by_vec)[i - 1].score, (*by_vec)[i].score);
          }
          for (const auto& result : *by_vec) {
            EXPECT_TRUE(IsKnownId(result.item_id)) << result.item_id;
          }
        }

        // SearchByIdVectors (query by existing ID; may not exist transiently)
        std::string id = "item" + std::to_string(id_dist(rng));
        auto by_id = engine.SearchByIdVectors(id, 10, {});
        // Either found-with-results or a clean not-found error.
        if (by_id.has_value()) {
          for (const auto& result : *by_id) {
            EXPECT_TRUE(IsKnownId(result.item_id)) << result.item_id;
            EXPECT_NE(result.item_id, id);
          }
        } else {
          EXPECT_EQ(by_id.error().code(), utils::ErrorCode::kVectorNotFound) << by_id.error().message();
        }

        read_ops.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }

  // Let writers finish, then signal readers to stop.
  for (int w = 0; w < kWriters; ++w) {
    threads[static_cast<size_t>(w)].join();
  }
  stop.store(true, std::memory_order_relaxed);
  for (int r = 0; r < kReaders; ++r) {
    threads[static_cast<size_t>(kWriters + r)].join();
  }

  EXPECT_GT(read_ops.load(), 0U);
}

TEST(VectorStoreConcurrencyStress, FlatBruteForce) {
  RunStress("flat", /*exercise_dispatcher_path=*/false);
}

TEST(VectorStoreConcurrencyStress, HnswIncrementalInsert) {
  // A compacting delete or an interleaved write leaves the index a generation
  // behind, which rebuilds the whole graph. With ef_construction neighbour
  // selection over the shared ID space that costs orders of magnitude more per
  // write than the flat or IVF publish, hence the shorter write run; the
  // interleaving being exercised is the same.
  RunStress("hnsw", /*exercise_dispatcher_path=*/true, /*iterations=*/24);
}

TEST(VectorStoreConcurrencyStress, IvfWithTraining) {
  RunStress("ivf", /*exercise_dispatcher_path=*/true);
}

/// Dimension used by the store while the mismatch race below is running.
constexpr uint32_t kRaceStoredDim = 64;

/// Query length that agrees with no stored vector, so no scan may score it.
constexpr uint32_t kRaceQueryDim = 3;

// Drive SearchByVector with a query that matches no stored vector while a writer
// repeatedly empties and refills the store.
//
// Clear() resets the store's dimension to 0, which is what makes a dimension
// check taken before the scan unsound: a query of any length passes while the
// store is momentarily empty, and a concurrent refill re-establishes a dimension
// the query does not have. The observable consequence is scoring a short query
// against wide rows, so the assertion is that a mismatched query never yields a
// result: it is either rejected or answered against an empty store.
void RunDimensionMismatchRace(const std::string& index_type, int cycles) {
  auto vectors_config = MakeVectorsConfig();
  vectors_config.default_dimension = kRaceStoredDim;
  auto similarity_config = MakeSimilarityConfig(index_type);

  events::EventStore event_store(config::EventsConfig{});
  events::CoOccurrenceIndex co_index;
  vectors::VectorStore vector_store(vectors_config);
  SimilarityEngine engine(&event_store, &co_index, &vector_store, similarity_config, vectors_config);

  constexpr size_t kFillCount = 64;
  std::atomic<bool> stop{false};
  std::atomic<size_t> scored_results{0};
  std::atomic<size_t> read_ops{0};

  std::thread writer([&]() {
    std::mt19937 rng(7);
    for (int cycle = 0; cycle < cycles; ++cycle) {
      vector_store.Clear();
      for (size_t i = 0; i < kFillCount; ++i) {
        auto vec = MakeVector(rng, kRaceStoredDim);
        const std::string id = "item" + std::to_string(i);
        if (vector_store.SetVector(id, vec).has_value()) {
          auto idx = vector_store.GetCompactIndex(id);
          if (idx.has_value()) {
            engine.NotifyVectorAdded(idx.value(), vec.data());
          }
        }
      }
    }
    stop.store(true, std::memory_order_relaxed);
  });

  std::vector<std::thread> readers;
  constexpr int kReaders = 4;
  for (int r = 0; r < kReaders; ++r) {
    readers.emplace_back([&, r]() {
      std::mt19937 rng(static_cast<uint32_t>(900 + r));
      const std::vector<float> short_query = MakeVector(rng, kRaceQueryDim);
      while (!stop.load(std::memory_order_relaxed)) {
        auto result = engine.SearchByVector(short_query, 10, {});
        if (result.has_value() && !result->empty()) {
          scored_results.fetch_add(result->size(), std::memory_order_relaxed);
        }
        read_ops.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }

  writer.join();
  for (auto& reader : readers) {
    reader.join();
  }

  EXPECT_GT(read_ops.load(), 0U);
  EXPECT_EQ(scored_results.load(), 0U) << "a query of " << kRaceQueryDim << " components was scored against "
                                       << kRaceStoredDim << "-dimensional vectors";
}

TEST(VectorStoreConcurrencyStress, FlatRejectsMismatchedQueryDuringRefill) {
  RunDimensionMismatchRace("flat", /*cycles=*/300);
}

TEST(VectorStoreConcurrencyStress, HnswRejectsMismatchedQueryDuringRefill) {
  // Each refill rebuilds the graph, so fewer cycles buy the same interleaving.
  RunDimensionMismatchRace("hnsw", /*cycles=*/60);
}

TEST(VectorStoreConcurrencyStress, IvfRejectsMismatchedQueryDuringRefill) {
  RunDimensionMismatchRace("ivf", /*cycles=*/300);
}

}  // namespace
}  // namespace nvecd::similarity
