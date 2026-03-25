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

  // Note: normalized flag is not tracked in compact storage
  // The data should still be normalized though

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

// ============================================================================
// Tombstone and Defragmentation
// ============================================================================

TEST(VectorStoreTest, DeleteCreatesTombstone) {
  auto config = MakeConfig();
  VectorStore store(config);

  std::vector<float> vec1 = {1.0f, 2.0f, 3.0f};
  std::vector<float> vec2 = {4.0f, 5.0f, 6.0f};
  std::vector<float> vec3 = {7.0f, 8.0f, 9.0f};

  ASSERT_TRUE(store.SetVector("item1", vec1).has_value());
  ASSERT_TRUE(store.SetVector("item2", vec2).has_value());
  ASSERT_TRUE(store.SetVector("item3", vec3).has_value());

  // Delete middle item
  EXPECT_TRUE(store.DeleteVector("item2"));

  // Active count should be 2
  EXPECT_EQ(store.GetVectorCount(), 2);

  // Deleted item should not be accessible
  EXPECT_FALSE(store.HasVector("item2"));
  EXPECT_FALSE(store.GetVector("item2").has_value());

  // Remaining items should still be accessible
  EXPECT_TRUE(store.HasVector("item1"));
  EXPECT_TRUE(store.HasVector("item3"));

  auto retrieved1 = store.GetVector("item1");
  ASSERT_TRUE(retrieved1.has_value());
  EXPECT_EQ(retrieved1->data, vec1);

  auto retrieved3 = store.GetVector("item3");
  ASSERT_TRUE(retrieved3.has_value());
  EXPECT_EQ(retrieved3->data, vec3);
}

TEST(VectorStoreTest, AutoDefragmentAt25PercentThreshold) {
  auto config = MakeConfig();
  VectorStore store(config);

  // Insert 8 items
  for (int i = 0; i < 8; ++i) {
    std::vector<float> vec = {static_cast<float>(i), static_cast<float>(i + 1),
                              static_cast<float>(i + 2)};
    ASSERT_TRUE(store.SetVector("item" + std::to_string(i), vec).has_value());
  }
  EXPECT_EQ(store.GetVectorCount(), 8);

  // Delete 2 of 8 = 25%, threshold is >, so no defrag yet.
  // Compact count should still include tombstones.
  EXPECT_TRUE(store.DeleteVector("item1"));
  EXPECT_TRUE(store.DeleteVector("item3"));
  EXPECT_EQ(store.GetVectorCount(), 6);

  // With 2 tombstones out of 8 total slots (25%), auto-defrag should NOT
  // have triggered yet (condition is tombstone_count * 4 > total_slots,
  // i.e. strictly greater than 25%).
  // Compact count still 8 (tombstones remain).
  {
    auto lock = store.AcquireReadLock();
    EXPECT_EQ(store.GetCompactCount(), 8);
  }

  // Delete a third item: 3 tombstones out of 8 total (37.5% > 25%).
  // This should trigger auto-defragmentation.
  EXPECT_TRUE(store.DeleteVector("item5"));
  EXPECT_EQ(store.GetVectorCount(), 5);

  // After auto-defrag, compact storage should be clean (no tombstones).
  {
    auto lock = store.AcquireReadLock();
    EXPECT_EQ(store.GetCompactCount(), 5);
  }

  // Verify remaining items are still accessible
  for (int i : {0, 2, 4, 6, 7}) {
    EXPECT_TRUE(store.HasVector("item" + std::to_string(i)))
        << "item" << i << " should still exist";
  }
}

TEST(VectorStoreTest, DefragmentPreservesDataIntegrity) {
  auto config = MakeConfig();
  VectorStore store(config);

  // Insert 10 items with known data
  std::vector<std::vector<float>> expected_data(10);
  for (int i = 0; i < 10; ++i) {
    expected_data[i] = {static_cast<float>(i * 10), static_cast<float>(i * 10 + 1),
                        static_cast<float>(i * 10 + 2), static_cast<float>(i * 10 + 3)};
    ASSERT_TRUE(store.SetVector("vec" + std::to_string(i), expected_data[i]).has_value());
  }

  // Delete 5 scattered items: indices 1, 3, 5, 7, 9
  for (int i : {1, 3, 5, 7, 9}) {
    EXPECT_TRUE(store.DeleteVector("vec" + std::to_string(i)));
  }
  EXPECT_EQ(store.GetVectorCount(), 5);

  // Explicit defragment
  store.Defragment();

  // Verify remaining 5 items have correct data
  for (int i : {0, 2, 4, 6, 8}) {
    auto retrieved = store.GetVector("vec" + std::to_string(i));
    ASSERT_TRUE(retrieved.has_value()) << "vec" << i << " should exist after defragment";
    EXPECT_EQ(retrieved->data, expected_data[i])
        << "vec" << i << " data mismatch after defragment";
  }

  // Verify deleted items are still gone
  for (int i : {1, 3, 5, 7, 9}) {
    EXPECT_FALSE(store.HasVector("vec" + std::to_string(i)))
        << "vec" << i << " should not exist after defragment";
  }

  // Compact storage should have no tombstones
  {
    auto lock = store.AcquireReadLock();
    EXPECT_EQ(store.GetCompactCount(), 5);
  }
}

TEST(VectorStoreTest, GetAllIdsSkipsTombstones) {
  auto config = MakeConfig();
  VectorStore store(config);

  std::vector<float> vec = {0.1f, 0.2f, 0.3f};
  ASSERT_TRUE(store.SetVector("alpha", vec).has_value());
  ASSERT_TRUE(store.SetVector("beta", vec).has_value());
  ASSERT_TRUE(store.SetVector("gamma", vec).has_value());
  ASSERT_TRUE(store.SetVector("delta", vec).has_value());
  ASSERT_TRUE(store.SetVector("epsilon", vec).has_value());

  // Delete 2 items
  EXPECT_TRUE(store.DeleteVector("beta"));
  EXPECT_TRUE(store.DeleteVector("delta"));

  auto ids = store.GetAllIds();
  EXPECT_EQ(ids.size(), 3);

  std::sort(ids.begin(), ids.end());
  EXPECT_EQ(ids[0], "alpha");
  EXPECT_EQ(ids[1], "epsilon");
  EXPECT_EQ(ids[2], "gamma");
}

TEST(VectorStoreTest, DeleteAndReinsertAfterDefragment) {
  auto config = MakeConfig();
  VectorStore store(config);

  // Insert initial items
  std::vector<float> vec_a = {1.0f, 2.0f, 3.0f};
  std::vector<float> vec_b = {4.0f, 5.0f, 6.0f};
  std::vector<float> vec_c = {7.0f, 8.0f, 9.0f};
  std::vector<float> vec_d = {10.0f, 11.0f, 12.0f};

  ASSERT_TRUE(store.SetVector("a", vec_a).has_value());
  ASSERT_TRUE(store.SetVector("b", vec_b).has_value());
  ASSERT_TRUE(store.SetVector("c", vec_c).has_value());
  ASSERT_TRUE(store.SetVector("d", vec_d).has_value());
  EXPECT_EQ(store.GetVectorCount(), 4);

  // Delete some items
  EXPECT_TRUE(store.DeleteVector("a"));
  EXPECT_TRUE(store.DeleteVector("c"));
  EXPECT_EQ(store.GetVectorCount(), 2);

  // Explicit defragment
  store.Defragment();
  EXPECT_EQ(store.GetVectorCount(), 2);

  // Reinsert new items (including a previously deleted ID)
  std::vector<float> vec_a_new = {100.0f, 200.0f, 300.0f};
  std::vector<float> vec_e = {13.0f, 14.0f, 15.0f};
  ASSERT_TRUE(store.SetVector("a", vec_a_new).has_value());
  ASSERT_TRUE(store.SetVector("e", vec_e).has_value());
  EXPECT_EQ(store.GetVectorCount(), 4);

  // Verify data integrity for all items
  auto retrieved_a = store.GetVector("a");
  ASSERT_TRUE(retrieved_a.has_value());
  EXPECT_EQ(retrieved_a->data, vec_a_new);  // New data, not old

  auto retrieved_b = store.GetVector("b");
  ASSERT_TRUE(retrieved_b.has_value());
  EXPECT_EQ(retrieved_b->data, vec_b);

  auto retrieved_d = store.GetVector("d");
  ASSERT_TRUE(retrieved_d.has_value());
  EXPECT_EQ(retrieved_d->data, vec_d);

  auto retrieved_e = store.GetVector("e");
  ASSERT_TRUE(retrieved_e.has_value());
  EXPECT_EQ(retrieved_e->data, vec_e);

  // Deleted item "c" should still be gone
  EXPECT_FALSE(store.HasVector("c"));

  // All IDs should be correct
  auto ids = store.GetAllIds();
  EXPECT_EQ(ids.size(), 4);
  std::sort(ids.begin(), ids.end());
  EXPECT_EQ(ids[0], "a");
  EXPECT_EQ(ids[1], "b");
  EXPECT_EQ(ids[2], "d");
  EXPECT_EQ(ids[3], "e");
}

TEST(VectorStoreTest, ConcurrentDeleteAndRead) {
  auto config = MakeConfig();
  VectorStore store(config);

  // Pre-populate with items
  constexpr int num_items = 200;
  std::vector<float> vec = {1.0f, 2.0f, 3.0f};
  for (int i = 0; i < num_items; ++i) {
    ASSERT_TRUE(store.SetVector("item" + std::to_string(i), vec).has_value());
  }

  std::atomic<bool> stop{false};
  std::atomic<int> read_count{0};
  std::atomic<int> delete_count{0};

  // Deleter thread: deletes items from the front
  std::thread deleter([&store, &stop, &delete_count]() {
    for (int i = 0; i < num_items && !stop.load(); ++i) {
      if (store.DeleteVector("item" + std::to_string(i))) {
        delete_count.fetch_add(1);
      }
      std::this_thread::sleep_for(std::chrono::microseconds(5));
    }
  });

  // Reader threads: read items and check consistency
  std::vector<std::thread> readers;
  for (int t = 0; t < 4; ++t) {
    readers.emplace_back([&store, &stop, &read_count]() {
      while (!stop.load()) {
        // Read operations should not crash regardless of concurrent deletes
        store.GetVectorCount();
        store.GetAllIds();

        for (int i = 0; i < num_items; i += 10) {
          auto result = store.GetVector("item" + std::to_string(i));
          if (result.has_value()) {
            // If we got a result, data should be valid
            EXPECT_EQ(result->data.size(), 3);
            read_count.fetch_add(1);
          }
          store.HasVector("item" + std::to_string(i));
        }
      }
    });
  }

  // Run for a short time
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  stop.store(true);

  deleter.join();
  for (auto& reader : readers) {
    reader.join();
  }

  // Verify consistency: count matches expectations
  EXPECT_GT(delete_count.load(), 0);
  EXPECT_GT(read_count.load(), 0);

  size_t remaining = store.GetVectorCount();
  auto ids = store.GetAllIds();
  EXPECT_EQ(ids.size(), remaining);
}

}  // namespace
}  // namespace nvecd::vectors
