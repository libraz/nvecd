/**
 * @file request_dispatcher_test.cpp
 * @brief Unit tests for RequestDispatcher command routing and auth
 */

#include "server/request_dispatcher.h"

#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <string>

#include "cache/similarity_cache.h"
#include "config/config.h"
#include "config/runtime_variable_manager.h"
#include "events/co_occurrence_index.h"
#include "events/event_store.h"
#include "server/server_types.h"
#include "similarity/similarity_engine.h"
#include "vectors/metadata_store.h"
#include "vectors/vector_store.h"

using namespace nvecd::server;

// ============================================================================
// Test fixture (mirrors handler_test.cpp pattern)
// ============================================================================

class RequestDispatcherTest : public ::testing::Test {
 protected:
  void SetUp() override {
    config_ = std::make_unique<nvecd::config::Config>();

    nvecd::config::EventsConfig events_cfg;
    event_store_ = std::make_unique<nvecd::events::EventStore>(events_cfg);

    co_index_ = std::make_unique<nvecd::events::CoOccurrenceIndex>();

    nvecd::config::VectorsConfig vectors_cfg;
    vector_store_ = std::make_unique<nvecd::vectors::VectorStore>(vectors_cfg);
    metadata_store_ = std::make_unique<nvecd::vectors::MetadataStore>();

    nvecd::config::SimilarityConfig sim_cfg;
    similarity_engine_ = std::make_unique<nvecd::similarity::SimilarityEngine>(
        event_store_.get(), co_index_.get(), vector_store_.get(), sim_cfg, vectors_cfg, metadata_store_.get());

    cache_ = std::make_unique<nvecd::cache::SimilarityCache>(1024 * 1024, 0, 0);

    auto manager_result = nvecd::config::RuntimeVariableManager::Create(*config_);
    ASSERT_TRUE(manager_result.has_value()) << "Failed to create RuntimeVariableManager";
    variable_manager_ = std::move(*manager_result);

    // Construct HandlerContext via placement new (contains atomics)
    ctx_ = new (ctx_storage_) HandlerContext{/*.event_store=*/event_store_.get(),
                                             /*.co_index=*/co_index_.get(),
                                             /*.vector_store=*/vector_store_.get(),
                                             /*.metadata_store=*/metadata_store_.get(),
                                             /*.similarity_engine=*/similarity_engine_.get(),
                                             /*.cache=*/{},
                                             /*.variable_manager=*/variable_manager_.get(),
                                             /*.stats=*/stats_,
                                             /*.config=*/config_.get(),
                                             /*.loading=*/loading_,
                                             /*.read_only=*/read_only_,
                                             /*.dump_dir=*/"/tmp",
                                             /*.requirepass=*/""};
    ctx_->cache.store(cache_.get(), std::memory_order_release);

    dispatcher_ = std::make_unique<RequestDispatcher>(*ctx_);
  }

  void TearDown() override {
    dispatcher_.reset();
    if (ctx_ != nullptr) {
      ctx_->~HandlerContext();
      ctx_ = nullptr;
    }
  }

  /// Helper: dispatch a command with a fresh ConnectionContext
  std::string Dispatch(const std::string& request) {
    ConnectionContext conn_ctx;
    conn_ctx.authenticated = false;
    return dispatcher_->Dispatch(request, conn_ctx);
  }

  /// Helper: dispatch with an explicit ConnectionContext
  std::string Dispatch(const std::string& request, ConnectionContext& conn_ctx) {
    return dispatcher_->Dispatch(request, conn_ctx);
  }

  ServerStats stats_;
  std::atomic<bool> loading_{false};
  std::atomic<bool> read_only_{false};
  std::unique_ptr<nvecd::config::Config> config_;
  std::unique_ptr<nvecd::events::EventStore> event_store_;
  std::unique_ptr<nvecd::events::CoOccurrenceIndex> co_index_;
  std::unique_ptr<nvecd::vectors::VectorStore> vector_store_;
  std::unique_ptr<nvecd::vectors::MetadataStore> metadata_store_;
  std::unique_ptr<nvecd::similarity::SimilarityEngine> similarity_engine_;
  std::unique_ptr<nvecd::cache::SimilarityCache> cache_;
  std::unique_ptr<nvecd::config::RuntimeVariableManager> variable_manager_;

  alignas(HandlerContext) char ctx_storage_[sizeof(HandlerContext)]{};  // NOLINT(modernize-avoid-c-arrays)
  HandlerContext* ctx_ = nullptr;
  std::unique_ptr<RequestDispatcher> dispatcher_;
};

// ============================================================================
// Basic dispatch tests
// ============================================================================

TEST_F(RequestDispatcherTest, UnknownCommandReturnsError) {
  auto response = Dispatch("FOOBAR\r\n");
  EXPECT_NE(response.find("ERROR"), std::string::npos);
}

TEST_F(RequestDispatcherTest, EmptyRequestReturnsError) {
  auto response = Dispatch("");
  EXPECT_NE(response.find("ERROR"), std::string::npos);
}

// ============================================================================
// Authentication tests
// ============================================================================

TEST_F(RequestDispatcherTest, AuthRequired_BlocksWriteWithoutAuth) {
  ctx_->requirepass = "secret123";

  ConnectionContext conn_ctx;
  conn_ctx.authenticated = false;

  auto response = Dispatch("EVENT ctx1 ADD item1 95\r\n", conn_ctx);
  EXPECT_NE(response.find("NOAUTH"), std::string::npos);
}

TEST_F(RequestDispatcherTest, AuthRequired_AllowsReadWithoutAuth) {
  ctx_->requirepass = "secret123";

  ConnectionContext conn_ctx;
  conn_ctx.authenticated = false;

  auto response = Dispatch("INFO\r\n", conn_ctx);
  // INFO is a read command; should not contain NOAUTH
  EXPECT_EQ(response.find("NOAUTH"), std::string::npos);
  // Should contain some info output (not an error about auth)
  EXPECT_NE(response.find("version:"), std::string::npos);
}

TEST_F(RequestDispatcherTest, AuthSuccess) {
  ctx_->requirepass = "correct_password";

  ConnectionContext conn_ctx;
  conn_ctx.authenticated = false;

  auto response = Dispatch("AUTH correct_password\r\n", conn_ctx);
  EXPECT_NE(response.find("OK"), std::string::npos);
  EXPECT_TRUE(conn_ctx.authenticated);
}

TEST_F(RequestDispatcherTest, AuthFailure) {
  ctx_->requirepass = "correct_password";

  ConnectionContext conn_ctx;
  conn_ctx.authenticated = false;

  auto response = Dispatch("AUTH wrong_password\r\n", conn_ctx);
  EXPECT_NE(response.find("ERROR"), std::string::npos);
  EXPECT_FALSE(conn_ctx.authenticated);
}

// ============================================================================
// Command response tests
// ============================================================================

TEST_F(RequestDispatcherTest, InfoCommandReturnsResponse) {
  auto response = Dispatch("INFO\r\n");
  // INFO should return server info with version and sections
  EXPECT_NE(response.find("version:"), std::string::npos);
  EXPECT_NE(response.find("# Server"), std::string::npos);
}

TEST_F(RequestDispatcherTest, StatsIncrementOnDispatch) {
  uint64_t before = stats_.total_commands.load();

  Dispatch("INFO\r\n");
  Dispatch("INFO\r\n");
  Dispatch("INFO\r\n");

  uint64_t after = stats_.total_commands.load();
  EXPECT_GE(after - before, 3u);
}

TEST_F(RequestDispatcherTest, SimvCachedResultStillAppliesMinScore) {
  nvecd::cache::CachePolicy policy;
  policy.enabled = true;
  cache_->SetSearchTypePolicy(nvecd::cache::SearchType::kVectorSearch, policy);

  ASSERT_NE(Dispatch("VECSET item1 1 0\r\n").find("OK"), std::string::npos);
  ASSERT_NE(Dispatch("VECSET item2 0 1\r\n").find("OK"), std::string::npos);

  auto warm_response = Dispatch("SIMV 2 min_score=0 1 0\r\n");
  ASSERT_NE(warm_response.find("OK RESULTS 2"), std::string::npos);
  ASSERT_NE(warm_response.find("item1"), std::string::npos);
  ASSERT_NE(warm_response.find("item2"), std::string::npos);

  auto filtered_cached_response = Dispatch("SIMV 2 min_score=0.5 1 0\r\n");
  EXPECT_NE(filtered_cached_response.find("OK RESULTS 1"), std::string::npos);
  EXPECT_NE(filtered_cached_response.find("item1"), std::string::npos);
  EXPECT_EQ(filtered_cached_response.find("item2"), std::string::npos);
}

TEST_F(RequestDispatcherTest, SimvCacheKeyUsesFullVector) {
  nvecd::cache::CachePolicy policy;
  policy.enabled = true;
  cache_->SetSearchTypePolicy(nvecd::cache::SearchType::kVectorSearch, policy);

  ASSERT_NE(Dispatch("VECSET positive_tail 0 0 0 0 1\r\n").find("OK"), std::string::npos);
  ASSERT_NE(Dispatch("VECSET negative_tail 0 0 0 0 -1\r\n").find("OK"), std::string::npos);

  auto positive_response = Dispatch("SIMV 1 0 0 0 0 1\r\n");
  ASSERT_NE(positive_response.find("OK RESULTS 1"), std::string::npos);
  ASSERT_NE(positive_response.find("positive_tail"), std::string::npos);

  auto negative_response = Dispatch("SIMV 1 0 0 0 0 -1\r\n");
  EXPECT_NE(negative_response.find("OK RESULTS 1"), std::string::npos);
  EXPECT_NE(negative_response.find("negative_tail"), std::string::npos);
  EXPECT_EQ(negative_response.find("positive_tail"), std::string::npos);
}

TEST_F(RequestDispatcherTest, SimFilterUsesMetadataStore) {
  ASSERT_NE(Dispatch("VECSET query 1 0\r\n").find("OK"), std::string::npos);
  ASSERT_NE(Dispatch("VECSET active 0.9 0.1\r\n").find("OK"), std::string::npos);
  ASSERT_NE(Dispatch("VECSET draft 0.8 0.2\r\n").find("OK"), std::string::npos);

  metadata_store_->Set("active", {{"status", std::string("active")}});
  metadata_store_->Set("draft", {{"status", std::string("draft")}});

  auto response = Dispatch("SIM query 10 using=vectors filter=status:draft\r\n");
  EXPECT_NE(response.find("OK RESULTS 1"), std::string::npos);
  EXPECT_NE(response.find("draft"), std::string::npos);
  EXPECT_EQ(response.find("active"), std::string::npos);
}

TEST_F(RequestDispatcherTest, MetasetStoresMetadataForFiltering) {
  ASSERT_NE(Dispatch("VECSET query 1 0\r\n").find("OK"), std::string::npos);
  ASSERT_NE(Dispatch("VECSET item1 0.9 0.1\r\n").find("OK"), std::string::npos);
  ASSERT_NE(Dispatch("VECSET item2 0.8 0.2\r\n").find("OK"), std::string::npos);

  EXPECT_NE(Dispatch("METASET item1 status:active,rank:10\r\n").find("OK METASET"), std::string::npos);
  EXPECT_NE(Dispatch("METASET item2 status:draft,rank:5\r\n").find("OK METASET"), std::string::npos);

  auto response = Dispatch("SIM query 10 using=vectors filter=status:active\r\n");
  EXPECT_NE(response.find("OK RESULTS 1"), std::string::npos);
  EXPECT_NE(response.find("item1"), std::string::npos);
  EXPECT_EQ(response.find("item2"), std::string::npos);
}

TEST_F(RequestDispatcherTest, MetasetRequiresExistingVector) {
  auto response = Dispatch("METASET missing status:active\r\n");
  EXPECT_NE(response.find("ERROR"), std::string::npos);
  EXPECT_NE(response.find("Vector not found"), std::string::npos);
}

TEST_F(RequestDispatcherTest, SimvFilterUsesMetadataStore) {
  ASSERT_NE(Dispatch("VECSET active 1 0\r\n").find("OK"), std::string::npos);
  ASSERT_NE(Dispatch("VECSET draft 0.9 0.1\r\n").find("OK"), std::string::npos);

  metadata_store_->Set("active", {{"status", std::string("active")}});
  metadata_store_->Set("draft", {{"status", std::string("draft")}});

  auto response = Dispatch("SIMV 10 filter=status:draft 1 0\r\n");
  EXPECT_NE(response.find("OK RESULTS 1"), std::string::npos);
  EXPECT_NE(response.find("draft"), std::string::npos);
  EXPECT_EQ(response.find("active"), std::string::npos);
}

// ============================================================================
// VECSET cache invalidation (new vector must appear in subsequent searches)
// ============================================================================

TEST_F(RequestDispatcherTest, VecsetInvalidatesCachedSimvResultsForNewVector) {
  nvecd::cache::CachePolicy policy;
  policy.enabled = true;
  cache_->SetSearchTypePolicy(nvecd::cache::SearchType::kVectorSearch, policy);

  ASSERT_NE(Dispatch("VECSET item1 1 0\r\n").find("OK"), std::string::npos);

  // Warm the cache for this query vector.
  auto warm = Dispatch("SIMV 10 min_score=0 1 0\r\n");
  ASSERT_NE(warm.find("OK RESULTS 1"), std::string::npos);
  ASSERT_NE(warm.find("item1"), std::string::npos);

  // Add a brand-new vector that should also match the query. The reverse index
  // cannot evict by item2 (no entry references it yet); the generation bump in
  // the cache key must invalidate the stale result.
  ASSERT_NE(Dispatch("VECSET item2 1 0\r\n").find("OK"), std::string::npos);

  auto again = Dispatch("SIMV 10 min_score=0 1 0\r\n");
  EXPECT_NE(again.find("OK RESULTS 2"), std::string::npos);
  EXPECT_NE(again.find("item1"), std::string::npos);
  EXPECT_NE(again.find("item2"), std::string::npos);
}

TEST_F(RequestDispatcherTest, VecsetInvalidatesCachedSimResultsForNewVector) {
  nvecd::cache::CachePolicy item_policy;
  item_policy.enabled = true;
  cache_->SetSearchTypePolicy(nvecd::cache::SearchType::kItemSearch, item_policy);

  ASSERT_NE(Dispatch("VECSET query 1 0\r\n").find("OK"), std::string::npos);
  ASSERT_NE(Dispatch("VECSET item1 1 0\r\n").find("OK"), std::string::npos);

  auto warm = Dispatch("SIM query 10 using=vectors min_score=0\r\n");
  ASSERT_NE(warm.find("OK RESULTS 1"), std::string::npos);

  ASSERT_NE(Dispatch("VECSET item2 1 0\r\n").find("OK"), std::string::npos);

  auto again = Dispatch("SIM query 10 using=vectors min_score=0\r\n");
  EXPECT_NE(again.find("OK RESULTS 2"), std::string::npos);
  EXPECT_NE(again.find("item2"), std::string::npos);
}

// ============================================================================
// Score formatting (fixed 4-decimal precision on the TCP surface)
// ============================================================================

TEST_F(RequestDispatcherTest, SimvScoresUseFixedFourDecimalPrecision) {
  ASSERT_NE(Dispatch("VECSET item1 1 0\r\n").find("OK"), std::string::npos);

  auto response = Dispatch("SIMV 1 min_score=0 1 0\r\n");
  ASSERT_NE(response.find("OK RESULTS 1"), std::string::npos);
  // A self-identical cosine score of 1.0 must render as "1.0000".
  EXPECT_NE(response.find("item1 1.0000"), std::string::npos);
}

// ============================================================================
// Events-mode filter over-fetch and vectorless eligibility (H-11)
// ============================================================================

TEST_F(RequestDispatcherTest, EventsFilterKeepsVectorlessMatchesAndReachesTopK) {
  // Build co-occurrence between "query" and neighbors via EVENT in one context.
  ASSERT_NE(Dispatch("EVENT ctx1 ADD query 50\r\n").find("OK"), std::string::npos);
  ASSERT_NE(Dispatch("EVENT ctx1 ADD keep1 50\r\n").find("OK"), std::string::npos);
  ASSERT_NE(Dispatch("EVENT ctx1 ADD drop1 50\r\n").find("OK"), std::string::npos);
  ASSERT_NE(Dispatch("EVENT ctx1 ADD keep2 50\r\n").find("OK"), std::string::npos);
  ASSERT_NE(Dispatch("EVENT ctx1 ADD drop2 50\r\n").find("OK"), std::string::npos);

  // Tag neighbors with metadata. None of them has a stored vector, exercising
  // the cold-start (vectorless) path: matching keys off the item ID only.
  metadata_store_->Set("keep1", {{"status", std::string("active")}});
  metadata_store_->Set("drop1", {{"status", std::string("draft")}});
  metadata_store_->Set("keep2", {{"status", std::string("active")}});
  metadata_store_->Set("drop2", {{"status", std::string("draft")}});

  // Ask for top_k=2 active items. Without over-fetch, filtering after a top-2
  // truncation could drop below 2; the over-fetch must still deliver both.
  auto response = Dispatch("SIM query 2 using=events filter=status:active\r\n");
  EXPECT_NE(response.find("OK RESULTS 2"), std::string::npos);
  EXPECT_NE(response.find("keep1"), std::string::npos);
  EXPECT_NE(response.find("keep2"), std::string::npos);
  EXPECT_EQ(response.find("drop1"), std::string::npos);
  EXPECT_EQ(response.find("drop2"), std::string::npos);
}

// ============================================================================
// Parse-time top_k validation wiring (#42)
// ============================================================================

TEST_F(RequestDispatcherTest, TopKAboveConfiguredMaxRejectedAtParseTime) {
  config_->similarity.max_top_k = 5;

  auto response = Dispatch("SIM item1 9999 using=vectors\r\n");
  EXPECT_NE(response.find("ERROR"), std::string::npos);
  EXPECT_NE(response.find("exceeds maximum allowed"), std::string::npos);
}

// ============================================================================
// DEBUG mode output (M-4)
// ============================================================================

TEST_F(RequestDispatcherTest, DebugModeAppendsDebugBlockToSim) {
  ConnectionContext conn_ctx;
  conn_ctx.authenticated = true;

  ASSERT_NE(Dispatch("VECSET item1 1 0\r\n", conn_ctx).find("OK"), std::string::npos);

  auto before = Dispatch("SIM item1 5 using=vectors\r\n", conn_ctx);
  EXPECT_EQ(before.find("# DEBUG"), std::string::npos);

  ASSERT_NE(Dispatch("DEBUG ON\r\n", conn_ctx).find("Debug mode enabled"), std::string::npos);
  EXPECT_TRUE(conn_ctx.debug_mode);

  auto after = Dispatch("SIM item1 5 using=vectors\r\n", conn_ctx);
  EXPECT_NE(after.find("# DEBUG"), std::string::npos);
  EXPECT_NE(after.find("query_time_us:"), std::string::npos);
  EXPECT_NE(after.find("mode: vectors"), std::string::npos);
}
