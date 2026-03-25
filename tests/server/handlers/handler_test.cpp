/**
 * @file handler_test.cpp
 * @brief Unit tests for server command handlers
 *
 * Tests all handler types: cache, info, debug, admin, and variable handlers.
 * Uses real objects for the main fixture (HandlerTest) to exercise actual
 * handler logic. A separate fixture (HandlerNullTest) tests null-pointer
 * error paths.
 */

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <string>

#include "cache/similarity_cache.h"
#include "config/config.h"
#include "config/runtime_variable_manager.h"
#include "events/co_occurrence_index.h"
#include "events/event_store.h"
#include "server/handlers/admin_handler.h"
#include "server/handlers/cache_handler.h"
#include "server/handlers/debug_handler.h"
#include "server/handlers/dump_handler.h"
#include "server/handlers/info_handler.h"
#include "server/handlers/variable_handler.h"
#include "server/server_types.h"
#include "similarity/similarity_engine.h"
#include "utils/error.h"
#include "vectors/vector_store.h"
#include "version.h"

using namespace nvecd::server;
using namespace nvecd::server::handlers;
using ::testing::HasSubstr;

// ============================================================================
// Main test fixture with real objects
// ============================================================================

class HandlerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // Create real objects for all handler dependencies
    config_ = std::make_unique<nvecd::config::Config>();

    nvecd::config::EventsConfig events_cfg;
    event_store_ = std::make_unique<nvecd::events::EventStore>(events_cfg);

    co_index_ = std::make_unique<nvecd::events::CoOccurrenceIndex>();

    nvecd::config::VectorsConfig vectors_cfg;
    vector_store_ = std::make_unique<nvecd::vectors::VectorStore>(vectors_cfg);

    nvecd::config::SimilarityConfig sim_cfg;
    similarity_engine_ = std::make_unique<nvecd::similarity::SimilarityEngine>(
        event_store_.get(), co_index_.get(), vector_store_.get(), sim_cfg, vectors_cfg);

    // Small cache (1MB) for testing
    cache_ = std::make_unique<nvecd::cache::SimilarityCache>(1024 * 1024, 0, 0);

    auto manager_result = nvecd::config::RuntimeVariableManager::Create(*config_);
    ASSERT_TRUE(manager_result.has_value()) << "Failed to create RuntimeVariableManager";
    variable_manager_ = std::move(*manager_result);

    // Construct HandlerContext in-place (non-copyable, non-movable due to atomics)
    ctx_ = new (ctx_storage_) HandlerContext{/*.event_store=*/event_store_.get(),
                                             /*.co_index=*/co_index_.get(),
                                             /*.vector_store=*/vector_store_.get(),
                                             /*.similarity_engine=*/similarity_engine_.get(),
                                             /*.cache=*/{},
                                             /*.variable_manager=*/variable_manager_.get(),
                                             /*.stats=*/stats_,
                                             /*.config=*/config_.get(),
                                             /*.loading=*/loading_,
                                             /*.read_only=*/read_only_,
                                             /*.dump_dir=*/"/tmp"};
    ctx_->cache.store(cache_.get(), std::memory_order_release);
  }

  void TearDown() override {
    if (ctx_ != nullptr) {
      ctx_->~HandlerContext();
      ctx_ = nullptr;
    }
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

  // Placement-new storage for HandlerContext (avoids copy/move of atomics)
  alignas(HandlerContext) char ctx_storage_[sizeof(HandlerContext)]{};  // NOLINT(modernize-avoid-c-arrays)
  HandlerContext* ctx_ = nullptr;
};

// ============================================================================
// Null-pointer test fixture for error path coverage
// ============================================================================

class HandlerNullTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // Minimal fixture with null pointers to test error paths
    nvecd::config::EventsConfig events_cfg;
    event_store_ = std::make_unique<nvecd::events::EventStore>(events_cfg);

    nvecd::config::VectorsConfig vectors_cfg;
    vector_store_ = std::make_unique<nvecd::vectors::VectorStore>(vectors_cfg);

    ctx_ = new (ctx_storage_) HandlerContext{/*.event_store=*/event_store_.get(),
                                             /*.co_index=*/nullptr,
                                             /*.vector_store=*/vector_store_.get(),
                                             /*.similarity_engine=*/nullptr,
                                             /*.cache=*/{},
                                             /*.variable_manager=*/nullptr,
                                             /*.stats=*/stats_,
                                             /*.config=*/nullptr,
                                             /*.loading=*/loading_,
                                             /*.read_only=*/read_only_,
                                             /*.dump_dir=*/""};
  }

  void TearDown() override {
    if (ctx_ != nullptr) {
      ctx_->~HandlerContext();
      ctx_ = nullptr;
    }
  }

  ServerStats stats_;
  std::atomic<bool> loading_{false};
  std::atomic<bool> read_only_{false};
  std::unique_ptr<nvecd::events::EventStore> event_store_;
  std::unique_ptr<nvecd::vectors::VectorStore> vector_store_;

  alignas(HandlerContext) char ctx_storage_[sizeof(HandlerContext)]{};  // NOLINT(modernize-avoid-c-arrays)
  HandlerContext* ctx_ = nullptr;
};

// ============================================================================
// Cache Handler Tests (real cache)
// ============================================================================

TEST_F(HandlerTest, CacheStats_WithCache_ReturnsStats) {
  auto result = HandleCacheStats(*ctx_);
  ASSERT_TRUE(result.has_value());
  EXPECT_THAT(*result, HasSubstr("cache_enabled: true"));
  EXPECT_THAT(*result, HasSubstr("cache_entries:"));
  EXPECT_THAT(*result, HasSubstr("cache_hits:"));
  EXPECT_THAT(*result, HasSubstr("cache_misses:"));
  EXPECT_THAT(*result, HasSubstr("cache_hit_rate:"));
  EXPECT_THAT(*result, HasSubstr("evictions:"));
}

TEST_F(HandlerTest, CacheClear_WithCache_Succeeds) {
  auto result = HandleCacheClear(*ctx_);
  ASSERT_TRUE(result.has_value());
  EXPECT_THAT(*result, HasSubstr("CACHE_CLEARED"));
}

TEST_F(HandlerTest, CacheEnable_WithCache_Enables) {
  cache_->SetEnabled(false);

  auto result = HandleCacheEnable(*ctx_);
  ASSERT_TRUE(result.has_value());
  EXPECT_THAT(*result, HasSubstr("CACHE_ENABLED"));
  EXPECT_TRUE(cache_->IsEnabled());
}

TEST_F(HandlerTest, CacheDisable_WithCache_Disables) {
  auto result = HandleCacheDisable(*ctx_);
  ASSERT_TRUE(result.has_value());
  EXPECT_THAT(*result, HasSubstr("CACHE_DISABLED"));
  EXPECT_FALSE(cache_->IsEnabled());
}

// ============================================================================
// Cache Handler Null Tests
// ============================================================================

TEST_F(HandlerNullTest, CacheStats_NullCache_ReturnsDisabled) {
  auto result = HandleCacheStats(*ctx_);
  ASSERT_TRUE(result.has_value());
  EXPECT_THAT(*result, HasSubstr("cache_enabled: false"));
  EXPECT_THAT(*result, HasSubstr("cache_entries: 0"));
}

TEST_F(HandlerNullTest, CacheClear_NullCache_ReturnsNoCache) {
  auto result = HandleCacheClear(*ctx_);
  ASSERT_TRUE(result.has_value());
  EXPECT_THAT(*result, HasSubstr("no cache"));
}

TEST_F(HandlerNullTest, CacheEnable_NullCache_ReturnsNoInstance) {
  auto result = HandleCacheEnable(*ctx_);
  ASSERT_TRUE(result.has_value());
  EXPECT_THAT(*result, HasSubstr("no cache instance"));
}

TEST_F(HandlerNullTest, CacheDisable_NullCache_ReturnsNoInstance) {
  auto result = HandleCacheDisable(*ctx_);
  ASSERT_TRUE(result.has_value());
  EXPECT_THAT(*result, HasSubstr("no cache instance"));
}

// ============================================================================
// Info Handler Tests (real objects)
// ============================================================================

TEST_F(HandlerTest, Info_ReturnsVersionFromVersionClass) {
  auto result = HandleInfo(*ctx_);
  ASSERT_TRUE(result.has_value());
  EXPECT_THAT(*result, HasSubstr("version: " + nvecd::Version::String()));
}

TEST_F(HandlerTest, Info_ContainsAllSections) {
  auto result = HandleInfo(*ctx_);
  ASSERT_TRUE(result.has_value());
  EXPECT_THAT(*result, HasSubstr("# Server"));
  EXPECT_THAT(*result, HasSubstr("# Stats"));
  EXPECT_THAT(*result, HasSubstr("# Memory"));
  EXPECT_THAT(*result, HasSubstr("# Cache"));
  EXPECT_THAT(*result, HasSubstr("# Data"));
}

TEST_F(HandlerTest, Info_ContainsUptimeAndCommandStats) {
  auto result = HandleInfo(*ctx_);
  ASSERT_TRUE(result.has_value());
  EXPECT_THAT(*result, HasSubstr("uptime_seconds:"));
  EXPECT_THAT(*result, HasSubstr("total_commands_processed:"));
  EXPECT_THAT(*result, HasSubstr("active_connections:"));
}

TEST_F(HandlerTest, Info_WithCache_ShowsCacheStats) {
  auto result = HandleInfo(*ctx_);
  ASSERT_TRUE(result.has_value());
  // With a real cache, stats should reflect "enabled" state
  EXPECT_THAT(*result, HasSubstr("cache_entries: 0"));
  EXPECT_THAT(*result, HasSubstr("cache_hits: 0"));
  EXPECT_THAT(*result, HasSubstr("cache_misses: 0"));
}

TEST_F(HandlerTest, Info_ContainsDynamicMemoryHealth) {
  auto result = HandleInfo(*ctx_);
  ASSERT_TRUE(result.has_value());
  EXPECT_THAT(*result, HasSubstr("memory_health:"));
  bool has_valid_health = result->find("memory_health: HEALTHY") != std::string::npos ||
                          result->find("memory_health: WARNING") != std::string::npos ||
                          result->find("memory_health: CRITICAL") != std::string::npos;
  EXPECT_TRUE(has_valid_health);
}

TEST_F(HandlerNullTest, Info_NullCache_ShowsZeroCacheStats) {
  auto result = HandleInfo(*ctx_);
  ASSERT_TRUE(result.has_value());
  EXPECT_THAT(*result, HasSubstr("cache_entries: 0"));
  EXPECT_THAT(*result, HasSubstr("cache_hits: 0"));
  EXPECT_THAT(*result, HasSubstr("cache_misses: 0"));
}

// ============================================================================
// Debug Handler Tests
// ============================================================================

TEST_F(HandlerTest, DebugOn_SetsDebugModeTrue) {
  ConnectionContext conn_ctx;
  conn_ctx.debug_mode = false;

  auto result = HandleDebugOn(conn_ctx);
  ASSERT_TRUE(result.has_value());
  EXPECT_THAT(*result, HasSubstr("DEBUG_ON"));
  EXPECT_TRUE(conn_ctx.debug_mode);
}

TEST_F(HandlerTest, DebugOff_SetsDebugModeFalse) {
  ConnectionContext conn_ctx;
  conn_ctx.debug_mode = true;

  auto result = HandleDebugOff(conn_ctx);
  ASSERT_TRUE(result.has_value());
  EXPECT_THAT(*result, HasSubstr("DEBUG_OFF"));
  EXPECT_FALSE(conn_ctx.debug_mode);
}

// ============================================================================
// Admin Handler Tests
// ============================================================================

TEST_F(HandlerTest, ConfigHelp_EmptyPath_ReturnsTopLevel) {
  auto result = HandleConfigHelp("");
  ASSERT_TRUE(result.has_value());
  EXPECT_THAT(*result, HasSubstr("+OK"));
}

TEST_F(HandlerTest, ConfigHelp_InvalidPath_ReturnsError) {
  auto result = HandleConfigHelp("nonexistent.invalid.path");
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), nvecd::utils::ErrorCode::kNotFound);
}

TEST_F(HandlerTest, ConfigVerify_EmptyFilepath_ReturnsError) {
  auto result = HandleConfigVerify("");
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), nvecd::utils::ErrorCode::kInvalidArgument);
  EXPECT_THAT(result.error().message(), HasSubstr("requires a filepath"));
}

TEST_F(HandlerTest, ConfigVerify_NonexistentFile_ReturnsError) {
  auto result = HandleConfigVerify("/tmp/nonexistent_config_12345.yaml");
  ASSERT_FALSE(result.has_value());
}

// ============================================================================
// Dump Handler Tests
// ============================================================================

TEST_F(HandlerNullTest, DumpSave_NullConfig_ReturnsError) {
  auto result = HandleDumpSave(*ctx_, "");
  ASSERT_FALSE(result.has_value());
  EXPECT_THAT(result.error().message(), HasSubstr("configuration is not available"));
}

TEST_F(HandlerNullTest, DumpSave_NullCoIndex_ReturnsError) {
  nvecd::config::Config config;
  ctx_->config = &config;
  // co_index is nullptr in HandlerNullTest fixture
  auto result = HandleDumpSave(*ctx_, "");
  ASSERT_FALSE(result.has_value());
  EXPECT_THAT(result.error().message(), HasSubstr("required stores not initialized"));
}

TEST_F(HandlerTest, DumpLoad_EmptyFilepath_ReturnsError) {
  auto result = HandleDumpLoad(*ctx_, "");
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), nvecd::utils::ErrorCode::kInvalidArgument);
  EXPECT_THAT(result.error().message(), HasSubstr("requires a filepath"));
}

TEST_F(HandlerTest, DumpVerify_EmptyFilepath_ReturnsError) {
  auto result = HandleDumpVerify("", "");
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), nvecd::utils::ErrorCode::kInvalidArgument);
  EXPECT_THAT(result.error().message(), HasSubstr("requires a filepath"));
}

TEST_F(HandlerTest, DumpInfo_EmptyFilepath_ReturnsError) {
  auto result = HandleDumpInfo("", "");
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), nvecd::utils::ErrorCode::kInvalidArgument);
  EXPECT_THAT(result.error().message(), HasSubstr("requires a filepath"));
}

TEST_F(HandlerTest, DumpVerify_NonexistentFile_ReturnsError) {
  auto result = HandleDumpVerify("/tmp", "/tmp/nonexistent_snapshot_12345.dmp");
  ASSERT_FALSE(result.has_value());
}

TEST_F(HandlerTest, DumpLoad_NonexistentFile_ReturnsError) {
  auto result = HandleDumpLoad(*ctx_, "/tmp/nonexistent_snapshot_12345.dmp");
  ASSERT_FALSE(result.has_value());
}

// ============================================================================
// Variable Handler Tests (real RuntimeVariableManager from fixture)
// ============================================================================

TEST_F(HandlerTest, HandleSet_WithManager_SetsVariable) {
  auto result = HandleSet(variable_manager_.get(), "logging.level", "debug");
  ASSERT_TRUE(result.has_value());
  EXPECT_THAT(*result, HasSubstr("OK"));
}

TEST_F(HandlerTest, HandleGet_WithManager_GetsVariable) {
  auto result = HandleGet(variable_manager_.get(), "logging.level");
  ASSERT_TRUE(result.has_value());
  // Response is a bulk string with the value
  EXPECT_THAT(*result, HasSubstr("$"));
}

TEST_F(HandlerTest, HandleShowVariables_WithManager_ReturnsList) {
  auto result = HandleShowVariables(variable_manager_.get());
  ASSERT_TRUE(result.has_value());
  // Response starts with array count
  EXPECT_THAT(*result, HasSubstr("*"));
}

TEST_F(HandlerTest, HandleShowVariables_WithPattern_FiltersResults) {
  auto result = HandleShowVariables(variable_manager_.get(), "logging.%");
  ASSERT_TRUE(result.has_value());
  EXPECT_THAT(*result, HasSubstr("logging."));
}

TEST_F(HandlerTest, HandleSet_ImmutableVariable_ReturnsError) {
  // Attempting to set an immutable variable should fail
  auto result = HandleSet(variable_manager_.get(), "api.tcp.port", "9999");
  ASSERT_FALSE(result.has_value());
}

// ============================================================================
// Variable Handler Null Tests
// ============================================================================

TEST_F(HandlerNullTest, HandleSet_NullManager_ReturnsError) {
  auto result = HandleSet(nullptr, "logging.level", "debug");
  ASSERT_FALSE(result.has_value());
  EXPECT_THAT(result.error().message(), HasSubstr("not initialized"));
}

TEST_F(HandlerNullTest, HandleGet_NullManager_ReturnsError) {
  auto result = HandleGet(nullptr, "logging.level");
  ASSERT_FALSE(result.has_value());
  EXPECT_THAT(result.error().message(), HasSubstr("not initialized"));
}

TEST_F(HandlerNullTest, HandleShowVariables_NullManager_ReturnsError) {
  auto result = HandleShowVariables(nullptr);
  ASSERT_FALSE(result.has_value());
  EXPECT_THAT(result.error().message(), HasSubstr("not initialized"));
}
