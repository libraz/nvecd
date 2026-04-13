/**
 * @file cache_policy_test.cpp
 * @brief Tests for per-search-type cache policies
 */

#include <gtest/gtest.h>

#include <string>
#include <thread>
#include <vector>

#include "cache/similarity_cache.h"

namespace nvecd::cache {
namespace {

class CachePolicyTest : public ::testing::Test {
 protected:
  void SetUp() override {
    cache_ = std::make_unique<SimilarityCache>(1024 * 1024,  // 1MB
                                               0.0,          // no min query cost
                                               300           // 5 min TTL
    );
  }

  CacheKey MakeKey(const std::string& str) {
    // Simple key generation for testing
    CacheKey key;
    key.hash_high = std::hash<std::string>{}(str);
    key.hash_low = key.hash_high ^ 0xDEADBEEF;
    return key;
  }

  std::vector<similarity::SimilarityResult> MakeResults(int count) {
    std::vector<similarity::SimilarityResult> results;
    for (int i = 0; i < count; ++i) {
      similarity::SimilarityResult r;
      r.item_id = "item_" + std::to_string(i);
      r.score = 1.0F - static_cast<float>(i) * 0.1F;
      results.push_back(r);
    }
    return results;
  }

  std::unique_ptr<SimilarityCache> cache_;
};

// ============================================================================
// Default Policies
// ============================================================================

TEST_F(CachePolicyTest, DefaultItemSearchEnabled) {
  auto policy = cache_->GetSearchTypePolicy(SearchType::kItemSearch);
  EXPECT_TRUE(policy.enabled);
}

TEST_F(CachePolicyTest, DefaultVectorSearchDisabled) {
  auto policy = cache_->GetSearchTypePolicy(SearchType::kVectorSearch);
  EXPECT_FALSE(policy.enabled);
}

TEST_F(CachePolicyTest, DefaultFilteredSearchEnabled) {
  auto policy = cache_->GetSearchTypePolicy(SearchType::kFilteredSearch);
  EXPECT_TRUE(policy.enabled);
  EXPECT_EQ(policy.ttl_seconds, 60);
}

// ============================================================================
// Item Search (enabled by default)
// ============================================================================

TEST_F(CachePolicyTest, ItemSearchInsertAndLookup) {
  auto key = MakeKey("sim_item1");
  auto results = MakeResults(3);

  EXPECT_TRUE(cache_->Insert(key, results, 5.0, SearchType::kItemSearch));

  auto cached = cache_->Lookup(key, SearchType::kItemSearch);
  ASSERT_TRUE(cached.has_value());
  EXPECT_EQ(cached->size(), 3U);
}

// ============================================================================
// Vector Search (disabled by default)
// ============================================================================

TEST_F(CachePolicyTest, VectorSearchInsertBlocked) {
  auto key = MakeKey("simv_vec1");
  auto results = MakeResults(3);

  // Insert should be rejected by policy
  EXPECT_FALSE(cache_->Insert(key, results, 5.0, SearchType::kVectorSearch));
}

TEST_F(CachePolicyTest, VectorSearchLookupReturnsNullopt) {
  auto key = MakeKey("simv_vec2");
  auto results = MakeResults(3);

  // Even if somehow inserted (via non-typed Insert), typed Lookup returns nullopt
  cache_->Insert(key, results, 5.0);  // Non-typed insert

  auto cached = cache_->Lookup(key, SearchType::kVectorSearch);
  EXPECT_FALSE(cached.has_value());
}

TEST_F(CachePolicyTest, VectorSearchEnabledAfterPolicyChange) {
  CachePolicy policy;
  policy.enabled = true;
  policy.ttl_seconds = 0;
  cache_->SetSearchTypePolicy(SearchType::kVectorSearch, policy);

  auto key = MakeKey("simv_vec3");
  auto results = MakeResults(3);

  EXPECT_TRUE(cache_->Insert(key, results, 5.0, SearchType::kVectorSearch));

  auto cached = cache_->Lookup(key, SearchType::kVectorSearch);
  ASSERT_TRUE(cached.has_value());
  EXPECT_EQ(cached->size(), 3U);
}

// ============================================================================
// Filtered Search
// ============================================================================

TEST_F(CachePolicyTest, FilteredSearchInsertAndLookup) {
  auto key = MakeKey("sim_filtered");
  auto results = MakeResults(2);

  EXPECT_TRUE(cache_->Insert(key, results, 5.0, SearchType::kFilteredSearch));

  auto cached = cache_->Lookup(key, SearchType::kFilteredSearch);
  ASSERT_TRUE(cached.has_value());
  EXPECT_EQ(cached->size(), 2U);
}

// ============================================================================
// Policy Changes at Runtime
// ============================================================================

TEST_F(CachePolicyTest, DisableItemSearch) {
  CachePolicy policy;
  policy.enabled = false;
  cache_->SetSearchTypePolicy(SearchType::kItemSearch, policy);

  auto key = MakeKey("disabled_item");
  auto results = MakeResults(3);

  EXPECT_FALSE(cache_->Insert(key, results, 5.0, SearchType::kItemSearch));
}

TEST_F(CachePolicyTest, PolicyChangeAffectsNewOperationsOnly) {
  auto key = MakeKey("before_change");
  auto results = MakeResults(3);

  // Insert while enabled
  EXPECT_TRUE(cache_->Insert(key, results, 5.0, SearchType::kItemSearch));

  // Disable item search
  CachePolicy policy;
  policy.enabled = false;
  cache_->SetSearchTypePolicy(SearchType::kItemSearch, policy);

  // Lookup should now return nullopt (policy check before lookup)
  auto cached = cache_->Lookup(key, SearchType::kItemSearch);
  EXPECT_FALSE(cached.has_value());

  // But non-typed lookup still works
  auto cached2 = cache_->Lookup(key);
  EXPECT_TRUE(cached2.has_value());
}

// ============================================================================
// Per-Type Statistics
// ============================================================================

TEST_F(CachePolicyTest, PerTypeStatsTracked) {
  auto key1 = MakeKey("stat_item");
  auto key2 = MakeKey("stat_filter");
  auto results = MakeResults(3);

  // Insert item search
  cache_->Insert(key1, results, 5.0, SearchType::kItemSearch);

  // Lookup item search (hit)
  cache_->Lookup(key1, SearchType::kItemSearch);

  // Lookup item search (miss)
  cache_->Lookup(MakeKey("nonexistent"), SearchType::kItemSearch);

  // Insert filtered search
  cache_->Insert(key2, results, 5.0, SearchType::kFilteredSearch);

  // Lookup filtered search (hit)
  (void)cache_->Lookup(key2, SearchType::kFilteredSearch);

  // Lookup vector search (blocked by policy)
  (void)cache_->Lookup(MakeKey("vec"), SearchType::kVectorSearch);

  auto stats = cache_->GetStatistics();

  EXPECT_EQ(stats.item_search_queries, 2U);
  EXPECT_EQ(stats.item_search_hits, 1U);
  EXPECT_EQ(stats.filtered_search_queries, 1U);
  EXPECT_EQ(stats.filtered_search_hits, 1U);
  EXPECT_EQ(stats.vector_search_queries, 0U);  // Blocked by policy, not counted
}

// ============================================================================
// Non-typed Operations Backward Compatibility
// ============================================================================

TEST_F(CachePolicyTest, NonTypedInsertIgnoresPolicy) {
  auto key = MakeKey("non_typed");
  auto results = MakeResults(3);

  // Non-typed Insert always works (backward compat)
  EXPECT_TRUE(cache_->Insert(key, results, 5.0));

  // Non-typed Lookup always works
  auto cached = cache_->Lookup(key);
  EXPECT_TRUE(cached.has_value());
}

}  // namespace
}  // namespace nvecd::cache
