/**
 * @file ann_contract_test.cpp
 * @brief Contract suite every AnnIndex implementation must pass
 *
 * The AnnIndex interface documents a lifecycle contract that used to live only
 * in each implementation's comments, so a fix applied to one implementation
 * could silently leave the other one broken. These tests are type-parameterized
 * over every implementation MakeAnnIndex can build, which makes that failure
 * mode structural: an implementation that satisfies only part of the contract
 * turns the suite red, and adding a new implementation runs the same clauses
 * against it without writing any new test.
 */

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_set>
#include <vector>

#include "config/config.h"
#include "vectors/ann_index.h"
#include "vectors/ann_index_factory.h"
#include "vectors/distance.h"
#include "vectors/vector_store.h"

namespace nvecd::vectors {
namespace {

/// Implementations the contract suite runs against. kFlat is brute-force and
/// has no AnnIndex object, so it is the only enumerator absent here.
using AnnImplementations = ::testing::Types<std::integral_constant<AnnIndexType, AnnIndexType::kHnsw>,
                                            std::integral_constant<AnnIndexType, AnnIndexType::kIvf>>;

/// Number of types in a gtest type list.
template <typename List>
struct TypeListSize;

template <typename... Ts>
struct TypeListSize<::testing::Types<Ts...>> {
  static constexpr size_t value = sizeof...(Ts);
};

static_assert(TypeListSize<AnnImplementations>::value + 1 == static_cast<size_t>(AnnIndexType::kCount),
              "Every AnnIndexType except kFlat must appear in AnnImplementations, so that a new ANN implementation "
              "is held to the same lifecycle contract as the existing ones.");

/// Name the parameterized tests after the configuration value they cover.
class AnnImplName {
 public:
  template <typename T>
  static std::string GetName(int /*index*/) {
    return AnnIndexTypeName(T::value);
  }
};

/**
 * @brief Drives an implementation the way the server does
 *
 * Vectors are written to a VectorStore and published to the index by compact
 * index, because the IVF adapter reads candidate rows back out of the store.
 * Searching through this fixture therefore exercises the same pairing of store
 * and index that a query hits in production.
 */
template <typename ImplTag>
class AnnContractTest : public ::testing::Test {
 protected:
  static constexpr AnnIndexType kType = ImplTag::value;
  static constexpr uint32_t kDim = 8;

  void SetUp() override { CreateIndex(kDim); }

  /// Build the index, provisionally sized to @p dimension.
  void CreateIndex(uint32_t dimension) {
    AnnIndexOptions options;
    options.dimension = dimension;
    options.distance_metric = "cosine";
    options.hnsw.m = 8;
    options.hnsw.ef_construction = 64;
    options.hnsw.ef_search = 64;
    options.ivf.nlist = 4;
    options.ivf.nprobe = 4;
    options.vector_store = &store_;
    index_ = MakeAnnIndex(kType, options);
    ASSERT_NE(index_, nullptr);
  }

  /// Write to the store without publishing to the index.
  uint32_t InsertStoreOnly(const std::string& id, const std::vector<float>& vec) {
    EXPECT_TRUE(store_.SetVector(id, vec).has_value());
    auto compact = store_.GetCompactIndex(id);
    EXPECT_TRUE(compact.has_value());
    return static_cast<uint32_t>(compact.value_or(0));
  }

  /// Publish a stored row to the index, as the engine's write path does.
  void Publish(uint32_t compact_index) {
    // The published row is read under the store's snapshot lock, which is
    // released before any Search: the IVF adapter takes its own snapshot.
    auto snap = store_.GetCompactSnapshot();
    index_->Add(compact_index, snap.matrix + compact_index * snap.dim);
  }

  /// Write to the store and publish to the index, as the engine does.
  uint32_t Insert(const std::string& id, const std::vector<float>& vec) {
    const uint32_t compact_index = InsertStoreOnly(id, vec);
    Publish(compact_index);
    return compact_index;
  }

  /// Rebuild the index from everything currently in the store.
  void RebuildFromStore() {
    auto snap = store_.GetCompactSnapshot();
    index_->Rebuild(snap.matrix, static_cast<uint32_t>(snap.count),
                    static_cast<uint32_t>(snap.dim == 0 ? kDim : snap.dim));
  }

  /// Unit vector pointing along @p axis.
  static std::vector<float> AxisVector(uint32_t axis, uint32_t dim = kDim) {
    std::vector<float> vec(dim, 0.0F);
    vec[axis % dim] = 1.0F;
    return vec;
  }

  static std::vector<float> RandomVector(std::mt19937& rng, uint32_t dim = kDim) {
    std::normal_distribution<float> dist(0.0F, 1.0F);
    std::vector<float> vec(dim);
    for (auto& component : vec) {
      component = dist(rng);
    }
    Normalize(vec);
    return vec;
  }

  static bool Contains(const std::vector<std::pair<uint32_t, float>>& results, uint32_t compact_index) {
    for (const auto& [index, score] : results) {
      if (index == compact_index) {
        return true;
      }
    }
    return false;
  }

  config::VectorsConfig vectors_config_;
  VectorStore store_{vectors_config_};
  std::unique_ptr<AnnIndex> index_;
};

TYPED_TEST_SUITE(AnnContractTest, AnnImplementations, AnnImplName);

// ============================================================================
// Rebuild binds the index to the store's dimension
// ============================================================================

// An index constructed for the configured default dimension must adopt the
// store's real dimension on the empty rebuild, which is how the caller binds it
// before the very first insert. Without the rebind, the insert below copies at
// the configured stride and reads past the stored row.
TYPED_TEST(AnnContractTest, EmptyRebuildAdoptsRequestedDimensionBeforeFirstInsert) {
  constexpr uint32_t kOtherDim = TestFixture::kDim * 2;
  this->CreateIndex(kOtherDim);
  ASSERT_EQ(this->index_->Dimension(), kOtherDim);

  const uint32_t first = this->InsertStoreOnly("a", TestFixture::AxisVector(0));
  this->index_->Rebuild(nullptr, 0, static_cast<uint32_t>(this->store_.GetDimension()));
  ASSERT_EQ(this->index_->Dimension(), TestFixture::kDim);

  this->Publish(first);

  auto query = TestFixture::AxisVector(0);
  auto results = this->index_->Search(query.data(), 2);
  ASSERT_FALSE(results.empty());
  EXPECT_EQ(results[0].first, first);
  EXPECT_NEAR(results[0].second, 1.0F, 0.01F);
}

// The same rebind has to work when the store already holds vectors, which is
// the startup path: load a snapshot, then rebuild the index from the store.
TYPED_TEST(AnnContractTest, RebuildFromStoreAdoptsStoreDimension) {
  this->CreateIndex(TestFixture::kDim * 2);

  const uint32_t first = this->InsertStoreOnly("a", TestFixture::AxisVector(0));
  this->InsertStoreOnly("b", TestFixture::AxisVector(1));
  this->RebuildFromStore();

  EXPECT_EQ(this->index_->Dimension(), TestFixture::kDim);
  EXPECT_EQ(this->index_->Dimension(), static_cast<uint32_t>(this->store_.GetDimension()));

  auto query = TestFixture::AxisVector(0);
  auto results = this->index_->Search(query.data(), 2);
  ASSERT_FALSE(results.empty());
  EXPECT_EQ(results[0].first, first);
}

// Rebuild replaces the index contents; entries published before it must not
// survive it, because compact indices are renumbered by store compaction.
TYPED_TEST(AnnContractTest, RebuildReplacesPreviousContents) {
  this->Insert("a", TestFixture::AxisVector(0));
  this->Insert("b", TestFixture::AxisVector(1));

  this->index_->Rebuild(nullptr, 0, TestFixture::kDim);

  EXPECT_EQ(this->index_->Size(), 0U);
  auto query = TestFixture::AxisVector(0);
  EXPECT_TRUE(this->index_->Search(query.data(), 4).empty());
}

// ============================================================================
// Degenerate arguments
// ============================================================================

TYPED_TEST(AnnContractTest, SearchWithZeroTopKReturnsEmpty) {
  for (uint32_t i = 0; i < 4; ++i) {
    this->Insert("item" + std::to_string(i), TestFixture::AxisVector(i));
  }

  auto query = TestFixture::AxisVector(0);
  EXPECT_TRUE(this->index_->Search(query.data(), 0).empty());
}

TYPED_TEST(AnnContractTest, SearchOnEmptyIndexReturnsEmpty) {
  auto query = TestFixture::AxisVector(0);
  EXPECT_TRUE(this->index_->Search(query.data(), 4).empty());
  EXPECT_TRUE(this->index_->Search(query.data(), 0).empty());
}

// ============================================================================
// Deletion
// ============================================================================

TYPED_TEST(AnnContractTest, SearchNeverReturnsDeletedIndices) {
  constexpr uint32_t kCount = 6;
  std::vector<uint32_t> indices;
  for (uint32_t i = 0; i < kCount; ++i) {
    indices.push_back(this->Insert("item" + std::to_string(i), TestFixture::AxisVector(i)));
  }

  this->index_->MarkDeleted(indices[1]);
  this->index_->MarkDeleted(indices[3]);

  auto query = TestFixture::AxisVector(1);
  auto results = this->index_->Search(query.data(), kCount);
  EXPECT_FALSE(TestFixture::Contains(results, indices[1]));
  EXPECT_FALSE(TestFixture::Contains(results, indices[3]));
  EXPECT_TRUE(TestFixture::Contains(results, indices[0]));
}

// Size() is the index's own view of liveness. It has to agree with the
// tombstones the caller set, and repeated or unknown deletions must leave it
// alone rather than wrapping an unsigned counter below zero.
TYPED_TEST(AnnContractTest, SizeTracksLiveEntriesAndNeverUnderflows) {
  constexpr uint32_t kCount = 5;
  std::vector<uint32_t> indices;
  for (uint32_t i = 0; i < kCount; ++i) {
    indices.push_back(this->Insert("item" + std::to_string(i), TestFixture::AxisVector(i)));
  }
  ASSERT_EQ(this->index_->Size(), kCount);

  this->index_->MarkDeleted(indices[0]);
  this->index_->MarkDeleted(indices[2]);
  EXPECT_EQ(this->index_->Size(), kCount - 2);

  // Repeating a deletion, and deleting something never added, are both no-ops.
  this->index_->MarkDeleted(indices[0]);
  this->index_->MarkDeleted(9999);
  EXPECT_EQ(this->index_->Size(), kCount - 2);

  for (uint32_t i = 0; i < kCount; ++i) {
    this->index_->MarkDeleted(indices[i]);
  }
  EXPECT_EQ(this->index_->Size(), 0U);
  this->index_->MarkDeleted(indices[0]);
  EXPECT_EQ(this->index_->Size(), 0U);
}

TYPED_TEST(AnnContractTest, DeletedIndexBecomesVisibleAgainAfterRebuild) {
  const uint32_t first = this->Insert("a", TestFixture::AxisVector(0));
  this->Insert("b", TestFixture::AxisVector(1));
  this->index_->MarkDeleted(first);

  this->RebuildFromStore();

  auto query = TestFixture::AxisVector(0);
  auto results = this->index_->Search(query.data(), 2);
  EXPECT_TRUE(TestFixture::Contains(results, first));
}

// ============================================================================
// Scoring
// ============================================================================

// A zero-norm vector has no defined cosine, and the implementations used to
// disagree on what to do with it: one scored it 0.0 and ranked it, the other
// dropped it. That made the set of items a query can return depend on which
// index was configured. The rule is a single one: score 0.0 and rank it.
TYPED_TEST(AnnContractTest, ZeroNormCandidateScoresZeroAndStillRanks) {
  const uint32_t aligned = this->Insert("aligned", TestFixture::AxisVector(0));
  const uint32_t other = this->Insert("other", TestFixture::AxisVector(1));
  const uint32_t zero = this->Insert("zero", std::vector<float>(TestFixture::kDim, 0.0F));

  auto query = TestFixture::AxisVector(0);
  auto results = this->index_->Search(query.data(), 3);

  ASSERT_EQ(results.size(), 3U);
  EXPECT_TRUE(TestFixture::Contains(results, aligned));
  EXPECT_TRUE(TestFixture::Contains(results, other));
  ASSERT_TRUE(TestFixture::Contains(results, zero));
  for (const auto& [index, score] : results) {
    if (index == zero) {
      EXPECT_FLOAT_EQ(score, 0.0F);
    }
  }
}

// A zero-norm query is equally undefined under cosine, and must not turn into
// an empty result set in one implementation and a full one in another.
TYPED_TEST(AnnContractTest, ZeroNormQueryScoresEveryCandidateZero) {
  for (uint32_t i = 0; i < 3; ++i) {
    this->Insert("item" + std::to_string(i), TestFixture::AxisVector(i));
  }

  std::vector<float> query(TestFixture::kDim, 0.0F);
  auto results = this->index_->Search(query.data(), 3);

  ASSERT_EQ(results.size(), 3U);
  for (const auto& [index, score] : results) {
    EXPECT_FLOAT_EQ(score, 0.0F);
  }
}

TYPED_TEST(AnnContractTest, ResultsAreSortedByScoreDescending) {
  std::mt19937 rng(1234);
  for (uint32_t i = 0; i < 32; ++i) {
    this->Insert("item" + std::to_string(i), TestFixture::RandomVector(rng));
  }

  auto query = TestFixture::RandomVector(rng);
  auto results = this->index_->Search(query.data(), 8);
  ASSERT_FALSE(results.empty());
  EXPECT_LE(results.size(), 8U);
  for (size_t i = 1; i < results.size(); ++i) {
    EXPECT_GE(results[i - 1].second, results[i].second);
  }

  std::unordered_set<uint32_t> seen;
  for (const auto& [index, score] : results) {
    EXPECT_TRUE(seen.insert(index).second) << "duplicate compact index in results";
  }
}

// ============================================================================
// Concurrency
// ============================================================================

// The interface promises concurrent Search while the caller mutates the index.
// Readers must keep seeing self-consistent, sorted, live results throughout.
TYPED_TEST(AnnContractTest, SearchStaysConsistentDuringConcurrentMutation) {
  std::mt19937 rng(7);
  std::vector<uint32_t> indices;
  for (uint32_t i = 0; i < 16; ++i) {
    indices.push_back(this->Insert("seed" + std::to_string(i), TestFixture::RandomVector(rng)));
  }

  std::atomic<bool> stop{false};
  std::atomic<size_t> searches{0};
  std::vector<std::thread> readers;
  for (int r = 0; r < 2; ++r) {
    readers.emplace_back([this, &stop, &searches, seed = r]() {
      std::mt19937 local_rng(static_cast<uint32_t>(seed) + 100U);
      while (!stop.load(std::memory_order_relaxed)) {
        auto query = TestFixture::RandomVector(local_rng);
        auto results = this->index_->Search(query.data(), 5);
        EXPECT_LE(results.size(), 5U);
        for (size_t i = 1; i < results.size(); ++i) {
          EXPECT_GE(results[i - 1].second, results[i].second);
        }
        searches.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }

  // Wait for the readers to actually be searching before mutating, so the
  // assertions above cover a concurrent window rather than a race the writer
  // happened to win.
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (searches.load(std::memory_order_relaxed) == 0 && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }
  const bool readers_running = searches.load(std::memory_order_relaxed) > 0;

  for (uint32_t i = 0; i < 64; ++i) {
    indices.push_back(this->Insert("item" + std::to_string(i), TestFixture::RandomVector(rng)));
    if (i % 4 == 0) {
      this->index_->MarkDeleted(indices[i]);
    }
  }

  stop.store(true, std::memory_order_relaxed);
  for (auto& reader : readers) {
    reader.join();
  }
  EXPECT_TRUE(readers_running) << "readers never searched; the window was not concurrent";
}

}  // namespace
}  // namespace nvecd::vectors
