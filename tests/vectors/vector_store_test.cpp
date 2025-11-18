/**
 * @file vector_store_test.cpp
 * @brief Unit tests for VectorStore
 */

#include "vectors/vector_store.h"

#include <gtest/gtest.h>

#include <thread>
#include <vector>

#include "vectors/distance.h"

namespace nvecd::vectors {
namespace {

// Helper to create default config
config::VectorsConfig MakeConfig() {
  config::VectorsConfig config;
  config.default_dimension = 768;
  config.distance_metric = "cosine";
  return config;
}

// ============================================================================
// Basic Operations
// ============================================================================

TEST(VectorStoreTest, ConstructEmpty) {
  auto config = MakeConfig();
  VectorStore store(config);

  EXPECT_EQ(store.GetVectorCount(), 0);
  EXPECT_EQ(store.GetDimension(), 0);
  EXPECT_TRUE(store.GetAllIds().empty());
}

TEST(VectorStoreTest, SetAndGetVector) {
  auto config = MakeConfig();
  VectorStore store(config);

  std::vector<float> vec = {0.1f, 0.2f, 0.3f};
  auto result = store.SetVector("item1", vec);
  ASSERT_TRUE(result.has_value()) << result.error().message();

  EXPECT_EQ(store.GetVectorCount(), 1);
  EXPECT_EQ(store.GetDimension(), 3);

  auto retrieved = store.GetVector("item1");
  ASSERT_TRUE(retrieved.has_value());
  EXPECT_EQ(retrieved->data, vec);
  EXPECT_FALSE(retrieved->normalized);
}

TEST(VectorStoreTest, SetMultipleVectors) {
  auto config = MakeConfig();
  VectorStore store(config);

  std::vector<float> vec1 = {0.1f, 0.2f, 0.3f};
  std::vector<float> vec2 = {0.4f, 0.5f, 0.6f};
  std::vector<float> vec3 = {0.7f, 0.8f, 0.9f};

  ASSERT_TRUE(store.SetVector("item1", vec1).has_value());
  ASSERT_TRUE(store.SetVector("item2", vec2).has_value());
  ASSERT_TRUE(store.SetVector("item3", vec3).has_value());

  EXPECT_EQ(store.GetVectorCount(), 3);
  EXPECT_EQ(store.GetDimension(), 3);

  auto retrieved1 = store.GetVector("item1");
  ASSERT_TRUE(retrieved1.has_value());
  EXPECT_EQ(retrieved1->data, vec1);

  auto retrieved2 = store.GetVector("item2");
  ASSERT_TRUE(retrieved2.has_value());
  EXPECT_EQ(retrieved2->data, vec2);
}

TEST(VectorStoreTest, OverwriteVector) {
  auto config = MakeConfig();
  VectorStore store(config);

  std::vector<float> vec1 = {0.1f, 0.2f, 0.3f};
  std::vector<float> vec2 = {0.4f, 0.5f, 0.6f};

  ASSERT_TRUE(store.SetVector("item1", vec1).has_value());
  ASSERT_TRUE(store.SetVector("item1", vec2).has_value());  // Overwrite

  EXPECT_EQ(store.GetVectorCount(), 1);

  auto retrieved = store.GetVector("item1");
  ASSERT_TRUE(retrieved.has_value());
  EXPECT_EQ(retrieved->data, vec2);  // Should have new value
}

// ============================================================================
// Dimension Validation
// ============================================================================

TEST(VectorStoreTest, DimensionMismatch) {
  auto config = MakeConfig();
  VectorStore store(config);

  std::vector<float> vec1 = {0.1f, 0.2f, 0.3f};  // 3D
  std::vector<float> vec2 = {0.4f, 0.5f};        // 2D

  ASSERT_TRUE(store.SetVector("item1", vec1).has_value());

  auto result = store.SetVector("item2", vec2);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), utils::ErrorCode::kVectorDimensionMismatch);

  EXPECT_EQ(store.GetVectorCount(), 1);  // Only first vector stored
}

TEST(VectorStoreTest, DimensionConsistency) {
  auto config = MakeConfig();
  VectorStore store(config);

  std::vector<float> vec1 = {0.1f, 0.2f, 0.3f, 0.4f};
  std::vector<float> vec2 = {0.5f, 0.6f, 0.7f, 0.8f};

  ASSERT_TRUE(store.SetVector("item1", vec1).has_value());
  EXPECT_EQ(store.GetDimension(), 4);

  ASSERT_TRUE(store.SetVector("item2", vec2).has_value());
  EXPECT_EQ(store.GetDimension(), 4);
}

// ============================================================================
// Normalization
// ============================================================================

TEST(VectorStoreTest, SetVectorWithNormalization) {
  auto config = MakeConfig();
  VectorStore store(config);

  std::vector<float> vec = {3.0f, 4.0f};  // Length = 5
  auto result = store.SetVector("item1", vec, true);
  ASSERT_TRUE(result.has_value());

  auto retrieved = store.GetVector("item1");
  ASSERT_TRUE(retrieved.has_value());
  EXPECT_TRUE(retrieved->normalized);

  // Check normalization: should be {0.6, 0.8}
  EXPECT_NEAR(retrieved->data[0], 0.6f, 1e-5);
  EXPECT_NEAR(retrieved->data[1], 0.8f, 1e-5);

  // Check L2 norm is 1
  float norm = L2Norm(retrieved->data);
  EXPECT_NEAR(norm, 1.0f, 1e-5);
}

TEST(VectorStoreTest, NormalizeZeroVector) {
  auto config = MakeConfig();
  VectorStore store(config);

  std::vector<float> zero_vec = {0.0f, 0.0f, 0.0f};
  auto result = store.SetVector("item1", zero_vec, true);

  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), utils::ErrorCode::kInvalidArgument);
  EXPECT_NE(result.error().message().find("zero vector"), std::string::npos);
}

// ============================================================================
// Validation Tests
// ============================================================================

TEST(VectorStoreTest, EmptyId) {
  auto config = MakeConfig();
  VectorStore store(config);

  std::vector<float> vec = {0.1f, 0.2f, 0.3f};
  auto result = store.SetVector("", vec);

  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), utils::ErrorCode::kInvalidArgument);
  EXPECT_NE(result.error().message().find("ID"), std::string::npos);
}

TEST(VectorStoreTest, EmptyVector) {
  auto config = MakeConfig();
  VectorStore store(config);

  std::vector<float> empty_vec;
  auto result = store.SetVector("item1", empty_vec);

  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), utils::ErrorCode::kInvalidArgument);
  EXPECT_NE(result.error().message().find("empty"), std::string::npos);
}

// ============================================================================
// Query Operations
// ============================================================================

TEST(VectorStoreTest, GetNonexistentVector) {
  auto config = MakeConfig();
  VectorStore store(config);

  auto result = store.GetVector("nonexistent");
  EXPECT_FALSE(result.has_value());
}

TEST(VectorStoreTest, HasVector) {
  auto config = MakeConfig();
  VectorStore store(config);

  std::vector<float> vec = {0.1f, 0.2f, 0.3f};
  ASSERT_TRUE(store.SetVector("item1", vec).has_value());

  EXPECT_TRUE(store.HasVector("item1"));
  EXPECT_FALSE(store.HasVector("nonexistent"));
}

TEST(VectorStoreTest, GetAllIds) {
  auto config = MakeConfig();
  VectorStore store(config);

  std::vector<float> vec = {0.1f, 0.2f, 0.3f};
  ASSERT_TRUE(store.SetVector("item1", vec).has_value());
  ASSERT_TRUE(store.SetVector("item2", vec).has_value());
  ASSERT_TRUE(store.SetVector("item3", vec).has_value());

  auto ids = store.GetAllIds();
  EXPECT_EQ(ids.size(), 3);

  std::sort(ids.begin(), ids.end());
  EXPECT_EQ(ids[0], "item1");
  EXPECT_EQ(ids[1], "item2");
  EXPECT_EQ(ids[2], "item3");
}

// ============================================================================
// Delete Operations
// ============================================================================

TEST(VectorStoreTest, DeleteVector) {
  auto config = MakeConfig();
  VectorStore store(config);

  std::vector<float> vec = {0.1f, 0.2f, 0.3f};
  ASSERT_TRUE(store.SetVector("item1", vec).has_value());

  EXPECT_TRUE(store.DeleteVector("item1"));
  EXPECT_EQ(store.GetVectorCount(), 0);
  EXPECT_FALSE(store.HasVector("item1"));
}

TEST(VectorStoreTest, DeleteNonexistentVector) {
  auto config = MakeConfig();
  VectorStore store(config);

  EXPECT_FALSE(store.DeleteVector("nonexistent"));
}

TEST(VectorStoreTest, DeleteAndReinsert) {
  auto config = MakeConfig();
  VectorStore store(config);

  std::vector<float> vec1 = {0.1f, 0.2f, 0.3f};
  std::vector<float> vec2 = {0.4f, 0.5f, 0.6f};

  ASSERT_TRUE(store.SetVector("item1", vec1).has_value());
  EXPECT_TRUE(store.DeleteVector("item1"));

  ASSERT_TRUE(store.SetVector("item1", vec2).has_value());

  auto retrieved = store.GetVector("item1");
  ASSERT_TRUE(retrieved.has_value());
  EXPECT_EQ(retrieved->data, vec2);
}

// ============================================================================
// Clear Operations
// ============================================================================

TEST(VectorStoreTest, ClearEmpty) {
  auto config = MakeConfig();
  VectorStore store(config);

  store.Clear();

  EXPECT_EQ(store.GetVectorCount(), 0);
  EXPECT_EQ(store.GetDimension(), 0);
}

TEST(VectorStoreTest, ClearWithData) {
  auto config = MakeConfig();
  VectorStore store(config);

  std::vector<float> vec = {0.1f, 0.2f, 0.3f};
  ASSERT_TRUE(store.SetVector("item1", vec).has_value());
  ASSERT_TRUE(store.SetVector("item2", vec).has_value());

  store.Clear();

  EXPECT_EQ(store.GetVectorCount(), 0);
  EXPECT_EQ(store.GetDimension(), 0);
  EXPECT_TRUE(store.GetAllIds().empty());
  EXPECT_FALSE(store.HasVector("item1"));
}

TEST(VectorStoreTest, ClearResetsDimension) {
  auto config = MakeConfig();
  VectorStore store(config);

  std::vector<float> vec1 = {0.1f, 0.2f, 0.3f};  // 3D
  ASSERT_TRUE(store.SetVector("item1", vec1).has_value());
  EXPECT_EQ(store.GetDimension(), 3);

  store.Clear();

  // After clear, should accept different dimension
  std::vector<float> vec2 = {0.1f, 0.2f, 0.3f, 0.4f};  // 4D
  ASSERT_TRUE(store.SetVector("item2", vec2).has_value());
  EXPECT_EQ(store.GetDimension(), 4);
}

// ============================================================================
// Concurrency Tests
// ============================================================================

TEST(VectorStoreTest, ConcurrentWrites) {
  auto config = MakeConfig();
  VectorStore store(config);

  constexpr int num_threads = 10;
  constexpr int vectors_per_thread = 100;

  std::vector<std::thread> threads;
  for (int t = 0; t < num_threads; ++t) {
    threads.emplace_back([&store, t]() {
      std::vector<float> vec = {static_cast<float>(t), 0.5f, 0.7f};
      for (int i = 0; i < vectors_per_thread; ++i) {
        std::string id = "item_" + std::to_string(t) + "_" + std::to_string(i);
        auto result = store.SetVector(id, vec);
        EXPECT_TRUE(result.has_value());
      }
    });
  }

  for (auto& thread : threads) {
    thread.join();
  }

  EXPECT_EQ(store.GetVectorCount(), num_threads * vectors_per_thread);
}

TEST(VectorStoreTest, ConcurrentReadsAndWrites) {
  auto config = MakeConfig();
  VectorStore store(config);

  // Initialize with some data
  std::vector<float> vec = {0.1f, 0.2f, 0.3f};
  for (int i = 0; i < 100; ++i) {
    ASSERT_TRUE(store.SetVector("item" + std::to_string(i), vec).has_value());
  }

  std::atomic<bool> stop{false};
  std::atomic<int> read_count{0};

  // Writer thread
  std::thread writer([&store, &stop]() {
    int counter = 100;
    std::vector<float> vec = {0.4f, 0.5f, 0.6f};
    while (!stop.load()) {
      store.SetVector("item" + std::to_string(counter++), vec);
      std::this_thread::sleep_for(std::chrono::microseconds(10));
    }
  });

  // Reader threads
  std::vector<std::thread> readers;
  for (int i = 0; i < 5; ++i) {
    readers.emplace_back([&store, &stop, &read_count]() {
      while (!stop.load()) {
        auto vec = store.GetVector("item0");
        if (vec) {
          read_count.fetch_add(1);
        }
        store.GetAllIds();
        store.GetVectorCount();
      }
    });
  }

  // Run for a short time
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  stop.store(true);

  writer.join();
  for (auto& reader : readers) {
    reader.join();
  }

  EXPECT_GT(read_count.load(), 0);
}

// ============================================================================
// Edge Cases
// ============================================================================

TEST(VectorStoreTest, LargeVectorDimension) {
  auto config = MakeConfig();
  VectorStore store(config);

  std::vector<float> large_vec(10000, 0.5f);
  auto result = store.SetVector("item1", large_vec);
  ASSERT_TRUE(result.has_value());

  EXPECT_EQ(store.GetDimension(), 10000);

  auto retrieved = store.GetVector("item1");
  ASSERT_TRUE(retrieved.has_value());
  EXPECT_EQ(retrieved->data.size(), 10000);
}

TEST(VectorStoreTest, VeryLongId) {
  auto config = MakeConfig();
  VectorStore store(config);

  std::string long_id(10000, 'a');
  std::vector<float> vec = {0.1f, 0.2f, 0.3f};

  auto result = store.SetVector(long_id, vec);
  ASSERT_TRUE(result.has_value());

  auto retrieved = store.GetVector(long_id);
  ASSERT_TRUE(retrieved.has_value());
  EXPECT_EQ(retrieved->data, vec);
}

TEST(VectorStoreTest, SpecialCharactersInId) {
  auto config = MakeConfig();
  VectorStore store(config);

  std::string special_id = "item@#$%^&*()";
  std::vector<float> vec = {0.1f, 0.2f, 0.3f};

  auto result = store.SetVector(special_id, vec);
  ASSERT_TRUE(result.has_value());

  auto retrieved = store.GetVector(special_id);
  ASSERT_TRUE(retrieved.has_value());
}

TEST(VectorStoreTest, NegativeValues) {
  auto config = MakeConfig();
  VectorStore store(config);

  std::vector<float> vec = {-0.1f, -0.2f, -0.3f};
  auto result = store.SetVector("item1", vec);
  ASSERT_TRUE(result.has_value());

  auto retrieved = store.GetVector("item1");
  ASSERT_TRUE(retrieved.has_value());
  EXPECT_EQ(retrieved->data, vec);
}

TEST(VectorStoreTest, MixedPositiveNegative) {
  auto config = MakeConfig();
  VectorStore store(config);

  std::vector<float> vec = {-0.5f, 0.0f, 0.5f, -1.0f, 1.0f};
  auto result = store.SetVector("item1", vec);
  ASSERT_TRUE(result.has_value());

  auto retrieved = store.GetVector("item1");
  ASSERT_TRUE(retrieved.has_value());
  EXPECT_EQ(retrieved->data, vec);
}

// ============================================================================
// High-Dimensional Vectors (Real-world LLM Embeddings)
// ============================================================================

TEST(VectorStoreTest, HighDimension_OpenAI_1536) {
  // OpenAI text-embedding-3-small uses 1536 dimensions
  auto config = MakeConfig();
  VectorStore store(config);

  std::vector<float> vec(1536);
  for (size_t i = 0; i < 1536; ++i) {
    vec[i] = static_cast<float>(i) / 1536.0f;
  }

  auto result = store.SetVector("openai_embedding", vec);
  ASSERT_TRUE(result.has_value()) << result.error().message();
  EXPECT_EQ(store.GetDimension(), 1536);

  auto retrieved = store.GetVector("openai_embedding");
  ASSERT_TRUE(retrieved.has_value());
  EXPECT_EQ(retrieved->data.size(), 1536);
  EXPECT_FLOAT_EQ(retrieved->data[0], 0.0f);
  EXPECT_FLOAT_EQ(retrieved->data[1535], 1535.0f / 1536.0f);
}

TEST(VectorStoreTest, HighDimension_Cohere_2048) {
  // Cohere embed-v3 uses up to 2048 dimensions
  auto config = MakeConfig();
  VectorStore store(config);

  std::vector<float> vec(2048, 0.5f);
  auto result = store.SetVector("cohere_embedding", vec);
  ASSERT_TRUE(result.has_value()) << result.error().message();
  EXPECT_EQ(store.GetDimension(), 2048);

  auto retrieved = store.GetVector("cohere_embedding");
  ASSERT_TRUE(retrieved.has_value());
  EXPECT_EQ(retrieved->data.size(), 2048);
}

TEST(VectorStoreTest, HighDimension_Claude_4096) {
  // Very high dimension (stress test)
  auto config = MakeConfig();
  VectorStore store(config);

  std::vector<float> vec(4096);
  // Create a pattern for verification
  for (size_t i = 0; i < 4096; ++i) {
    vec[i] = std::sin(static_cast<float>(i) * 0.01f);
  }

  auto result = store.SetVector("claude_embedding", vec);
  ASSERT_TRUE(result.has_value()) << result.error().message();
  EXPECT_EQ(store.GetDimension(), 4096);

  auto retrieved = store.GetVector("claude_embedding");
  ASSERT_TRUE(retrieved.has_value());
  EXPECT_EQ(retrieved->data.size(), 4096);

  // Verify pattern
  for (size_t i = 0; i < 4096; i += 100) {
    EXPECT_FLOAT_EQ(retrieved->data[i], std::sin(static_cast<float>(i) * 0.01f));
  }
}

TEST(VectorStoreTest, HighDimension_MultipleVectors) {
  // Test multiple high-dimensional vectors
  auto config = MakeConfig();
  VectorStore store(config);

  const size_t dim = 1536;
  const size_t count = 100;

  // Add 100 vectors of dimension 1536
  for (size_t i = 0; i < count; ++i) {
    std::vector<float> vec(dim);
    for (size_t j = 0; j < dim; ++j) {
      vec[j] = static_cast<float>(i * dim + j) / static_cast<float>(count * dim);
    }

    std::string id = "vec_" + std::to_string(i);
    auto result = store.SetVector(id, vec);
    ASSERT_TRUE(result.has_value()) << "Failed at vector " << i << ": " << result.error().message();
  }

  EXPECT_EQ(store.GetVectorCount(), count);
  EXPECT_EQ(store.GetDimension(), dim);

  // Verify a few vectors
  auto vec0 = store.GetVector("vec_0");
  ASSERT_TRUE(vec0.has_value());
  EXPECT_FLOAT_EQ(vec0->data[0], 0.0f);

  auto vec50 = store.GetVector("vec_50");
  ASSERT_TRUE(vec50.has_value());
  EXPECT_EQ(vec50->data.size(), dim);
}

TEST(VectorStoreTest, HighDimension_DimensionMismatch) {
  auto config = MakeConfig();
  VectorStore store(config);

  // First vector: 1536 dimensions
  std::vector<float> vec1(1536, 0.5f);
  auto result1 = store.SetVector("vec1", vec1);
  ASSERT_TRUE(result1.has_value());

  // Second vector: 2048 dimensions (should fail)
  std::vector<float> vec2(2048, 0.5f);
  auto result2 = store.SetVector("vec2", vec2);
  EXPECT_FALSE(result2.has_value());
  EXPECT_EQ(result2.error().code(), utils::ErrorCode::kVectorDimensionMismatch);
}

TEST(VectorStoreTest, HighDimension_Normalization) {
  auto config = MakeConfig();
  VectorStore store(config);

  // Create a 1536-dim vector with known norm
  std::vector<float> vec(1536, 1.0f);  // L2 norm = sqrt(1536) ≈ 39.19

  auto result = store.SetVector("unnormalized", vec, false);
  ASSERT_TRUE(result.has_value());
  auto retrieved = store.GetVector("unnormalized");
  ASSERT_TRUE(retrieved.has_value());
  EXPECT_FLOAT_EQ(retrieved->data[0], 1.0f);

  // With normalization
  auto result_norm = store.SetVector("normalized", vec, true);
  ASSERT_TRUE(result_norm.has_value());
  auto retrieved_norm = store.GetVector("normalized");
  ASSERT_TRUE(retrieved_norm.has_value());

  // Calculate L2 norm
  float norm = 0.0f;
  for (float val : retrieved_norm->data) {
    norm += val * val;
  }
  norm = std::sqrt(norm);
  EXPECT_NEAR(norm, 1.0f, 1e-5f);  // Should be normalized to 1.0
}

}  // namespace
}  // namespace nvecd::vectors
