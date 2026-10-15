/**
 * @file vector_store_compaction_generation_test.cpp
 * @brief The store generation must reveal a compaction, which re-keys every row
 */

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "vectors/vector_store.h"

namespace nvecd::vectors {
namespace {

class VectorStoreCompactionGenerationTest : public ::testing::Test {
 protected:
  void SetUp() override {
    config::VectorsConfig config;
    config.default_dimension = 3;
    config.distance_metric = "cosine";
    store_ = std::make_unique<VectorStore>(config);
    for (int i = 0; i < 8; ++i) {
      const float f = static_cast<float>(i);
      ASSERT_TRUE(store_->SetVector("item" + std::to_string(i), {f, f + 1.0F, f + 2.0F}).has_value());
    }
  }

  std::unique_ptr<VectorStore> store_;
};

TEST_F(VectorStoreCompactionGenerationTest, TombstoneDeleteAdvancesGenerationOnce) {
  const uint64_t before = store_->GetGeneration();
  ASSERT_TRUE(store_->DeleteVector("item1"));
  EXPECT_EQ(store_->GetGeneration(), before + 1);
  ASSERT_TRUE(store_->DeleteVector("item3"));
  EXPECT_EQ(store_->GetGeneration(), before + 2);
}

TEST_F(VectorStoreCompactionGenerationTest, DeleteThatCompactsAdvancesGenerationTwice) {
  ASSERT_TRUE(store_->DeleteVector("item1"));
  ASSERT_TRUE(store_->DeleteVector("item3"));
  const uint64_t before = store_->GetGeneration();

  // The third tombstone crosses the fragmentation threshold and compacts.
  ASSERT_TRUE(store_->DeleteVector("item5"));

  EXPECT_EQ(store_->GetGeneration(), before + 2);
  EXPECT_EQ(store_->GetCompactIndex("item6").value(), 3U);  // Re-keyed by the compaction
}

TEST_F(VectorStoreCompactionGenerationTest, DefragmentReportsWhetherRowsMoved) {
  const uint64_t clean = store_->GetGeneration();
  EXPECT_FALSE(store_->Defragment());
  EXPECT_EQ(store_->GetGeneration(), clean);

  ASSERT_TRUE(store_->DeleteVector("item2"));
  const uint64_t with_tombstone = store_->GetGeneration();
  EXPECT_TRUE(store_->Defragment());
  EXPECT_EQ(store_->GetGeneration(), with_tombstone + 1);
  EXPECT_FALSE(store_->Defragment());
}

}  // namespace
}  // namespace nvecd::vectors
