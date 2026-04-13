/**
 * @file request_dispatcher_test.cpp
 * @brief Unit tests for RequestDispatcher command routing and auth
 */

#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <string>

#include "cache/similarity_cache.h"
#include "config/config.h"
#include "config/runtime_variable_manager.h"
#include "events/co_occurrence_index.h"
#include "events/event_store.h"
#include "server/request_dispatcher.h"
#include "server/server_types.h"
#include "similarity/similarity_engine.h"
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

    nvecd::config::SimilarityConfig sim_cfg;
    similarity_engine_ = std::make_unique<nvecd::similarity::SimilarityEngine>(
        event_store_.get(), co_index_.get(), vector_store_.get(), sim_cfg, vectors_cfg);

    cache_ = std::make_unique<nvecd::cache::SimilarityCache>(1024 * 1024, 0, 0);

    auto manager_result = nvecd::config::RuntimeVariableManager::Create(*config_);
    ASSERT_TRUE(manager_result.has_value()) << "Failed to create RuntimeVariableManager";
    variable_manager_ = std::move(*manager_result);

    // Construct HandlerContext via placement new (contains atomics)
    ctx_ = new (ctx_storage_) HandlerContext{/*.event_store=*/event_store_.get(),
                                             /*.co_index=*/co_index_.get(),
                                             /*.vector_store=*/vector_store_.get(),
                                             /*.metadata_store=*/nullptr,
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
