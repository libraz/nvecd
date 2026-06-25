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

// Run a stress workload with the given index type and assert no crash.
void RunStress(const std::string& index_type, bool exercise_dispatcher_path) {
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
  constexpr int kIterations = 400;
  std::atomic<bool> stop{false};
  std::atomic<size_t> read_ops{0};

  std::vector<std::thread> threads;

  // Writer threads: set/delete overlapping IDs to force reallocation/defrag.
  for (int w = 0; w < kWriters; ++w) {
    threads.emplace_back([&, w]() {
      std::mt19937 rng(static_cast<uint32_t>(100 + w));
      std::uniform_int_distribution<size_t> id_dist(0, kIdSpace - 1);
      for (int iter = 0; iter < kIterations; ++iter) {
        size_t id_num = id_dist(rng);
        std::string id = "item" + std::to_string(id_num);
        if ((iter & 3) == 0) {
          vector_store.DeleteVector(id);
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
        }

        // SearchByIdVectors (query by existing ID; may not exist transiently)
        std::string id = "item" + std::to_string(id_dist(rng));
        auto by_id = engine.SearchByIdVectors(id, 10, {});
        // Either found-with-results or a clean not-found error; never a crash.
        (void)by_id;

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

TEST(VectorStoreConcurrencyStress, IvfWithTraining) {
  RunStress("ivf", /*exercise_dispatcher_path=*/true);
}

}  // namespace
}  // namespace nvecd::similarity
