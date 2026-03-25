/**
 * @file handler_test.cpp
 * @brief Unit tests for server command handlers
 *
 * Tests all handler types: cache, info, debug, admin, and variable handlers.
 */

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <string>

#include "cache/similarity_cache.h"
#include "config/config.h"
#include "config/runtime_variable_manager.h"
#include "events/event_store.h"
#include "server/handlers/admin_handler.h"
#include "server/handlers/cache_handler.h"
#include "server/handlers/debug_handler.h"
#include "server/handlers/dump_handler.h"
#include "server/handlers/info_handler.h"
#include "server/handlers/variable_handler.h"
#include "server/server_types.h"
#include "utils/error.h"
#include "vectors/vector_store.h"
#include "version.h"

using namespace nvecd::server;
using namespace nvecd::server::handlers;
using ::testing::HasSubstr;

// ============================================================================
// Test fixture with shared HandlerContext setup
// ============================================================================

class HandlerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // Create minimal real objects for handlers that need them
    nvecd::config::EventsConfig events_cfg;
    event_store_ = std::make_unique<nvecd::events::EventStore>(events_cfg);

    nvecd::config::VectorsConfig vectors_cfg;
    vector_store_ = std::make_unique<nvecd::vectors::VectorStore>(vectors_cfg);

    // Construct HandlerContext in-place (non-copyable, non-movable due to atomics)
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

  // Placement-new storage for HandlerContext (avoids copy/move of atomics)
  alignas(HandlerContext) char ctx_storage_[sizeof(HandlerContext)]{};  // NOLINT(modernize-avoid-c-arrays)
  HandlerContext* ctx_ = nullptr;
};

// ============================================================================
// Cache Handler Tests
// ============================================================================

TEST_F(HandlerTest, CacheStats_NullCache_ReturnsDisabled) {
  // cache is nullptr by default in ctx_
  auto result = HandleCacheStats(*ctx_);
  ASSERT_TRUE(result.has_value());
  EXPECT_THAT(*result, HasSubstr("cache_enabled: false"));
  EXPECT_THAT(*result, HasSubstr("cache_entries: 0"));
}

TEST_F(HandlerTest, CacheStats_WithCache_ReturnsStats) {
  auto cache = std::make_unique<nvecd::cache::SimilarityCache>(1024 * 1024, 0, 0);
  ctx_->cache.store(cache.get(), std::memory_order_release);

  auto result = HandleCacheStats(*ctx_);
  ASSERT_TRUE(result.has_value());
  EXPECT_THAT(*result, HasSubstr("cache_enabled:"));
  EXPECT_THAT(*result, HasSubstr("cache_entries:"));
  EXPECT_THAT(*result, HasSubstr("cache_hits:"));
  EXPECT_THAT(*result, HasSubstr("cache_misses:"));
  EXPECT_THAT(*result, HasSubstr("cache_hit_rate:"));
  EXPECT_THAT(*result, HasSubstr("evictions:"));
}

TEST_F(HandlerTest, CacheClear_NullCache_ReturnsNoCache) {
  auto result = HandleCacheClear(*ctx_);
  ASSERT_TRUE(result.has_value());
  EXPECT_THAT(*result, HasSubstr("no cache"));
}

TEST_F(HandlerTest, CacheClear_WithCache_Succeeds) {
  auto cache = std::make_unique<nvecd::cache::SimilarityCache>(1024 * 1024, 0, 0);
  ctx_->cache.store(cache.get(), std::memory_order_release);

  auto result = HandleCacheClear(*ctx_);
  ASSERT_TRUE(result.has_value());
  EXPECT_THAT(*result, HasSubstr("CACHE_CLEARED"));
}

TEST_F(HandlerTest, CacheEnable_NullCache_ReturnsNoInstance) {
  auto result = HandleCacheEnable(*ctx_);
  ASSERT_TRUE(result.has_value());
  EXPECT_THAT(*result, HasSubstr("no cache instance"));
}

TEST_F(HandlerTest, CacheEnable_WithCache_Enables) {
  auto cache = std::make_unique<nvecd::cache::SimilarityCache>(1024 * 1024, 0, 0);
  cache->SetEnabled(false);
  ctx_->cache.store(cache.get(), std::memory_order_release);

  auto result = HandleCacheEnable(*ctx_);
  ASSERT_TRUE(result.has_value());
  EXPECT_THAT(*result, HasSubstr("CACHE_ENABLED"));
  EXPECT_TRUE(cache->IsEnabled());
}

TEST_F(HandlerTest, CacheDisable_NullCache_ReturnsNoInstance) {
  auto result = HandleCacheDisable(*ctx_);
  ASSERT_TRUE(result.has_value());
  EXPECT_THAT(*result, HasSubstr("no cache instance"));
}

TEST_F(HandlerTest, CacheDisable_WithCache_Disables) {
  auto cache = std::make_unique<nvecd::cache::SimilarityCache>(1024 * 1024, 0, 0);
  ctx_->cache.store(cache.get(), std::memory_order_release);

  auto result = HandleCacheDisable(*ctx_);
  ASSERT_TRUE(result.has_value());
  EXPECT_THAT(*result, HasSubstr("CACHE_DISABLED"));
  EXPECT_FALSE(cache->IsEnabled());
}

// ============================================================================
// Info Handler Tests
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

TEST_F(HandlerTest, Info_NullCache_ShowsZeroCacheStats) {
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

TEST_F(HandlerTest, DumpSave_NullConfig_ReturnsError) {
  // ctx_ has config = nullptr by default
  auto result = HandleDumpSave(*ctx_, "");
  ASSERT_FALSE(result.has_value());
  EXPECT_THAT(result.error().message(), HasSubstr("configuration is not available"));
}

TEST_F(HandlerTest, DumpSave_NullStores_ReturnsError) {
  nvecd::config::Config config;
  ctx_->config = &config;
  // co_index is nullptr in our fixture setup
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
  nvecd::config::Config config;
  ctx_->config = &config;
  ctx_->dump_dir = "/tmp";
  auto result = HandleDumpLoad(*ctx_, "/tmp/nonexistent_snapshot_12345.dmp");
  ASSERT_FALSE(result.has_value());
}

// ============================================================================
// Info Handler Dynamic Memory Health Tests
// ============================================================================

TEST_F(HandlerTest, Info_ContainsDynamicMemoryHealth) {
  auto result = HandleInfo(*ctx_);
  ASSERT_TRUE(result.has_value());
  EXPECT_THAT(*result, HasSubstr("memory_health:"));
  bool has_valid_health = result->find("memory_health: HEALTHY") != std::string::npos ||
                          result->find("memory_health: WARNING") != std::string::npos ||
                          result->find("memory_health: CRITICAL") != std::string::npos;
  EXPECT_TRUE(has_valid_health);
}

// ============================================================================
// Variable Handler Tests
// ============================================================================

TEST_F(HandlerTest, HandleSet_NullManager_ReturnsError) {
  auto result = HandleSet(nullptr, "logging.level", "debug");
  ASSERT_FALSE(result.has_value());
  EXPECT_THAT(result.error().message(), HasSubstr("not initialized"));
}

TEST_F(HandlerTest, HandleGet_NullManager_ReturnsError) {
  auto result = HandleGet(nullptr, "logging.level");
  ASSERT_FALSE(result.has_value());
  EXPECT_THAT(result.error().message(), HasSubstr("not initialized"));
}

TEST_F(HandlerTest, HandleShowVariables_NullManager_ReturnsError) {
  auto result = HandleShowVariables(nullptr);
  ASSERT_FALSE(result.has_value());
  EXPECT_THAT(result.error().message(), HasSubstr("not initialized"));
}

TEST_F(HandlerTest, HandleSet_WithManager_SetsVariable) {
  nvecd::config::Config config;
  auto manager_result = nvecd::config::RuntimeVariableManager::Create(config);
  ASSERT_TRUE(manager_result.has_value());
  auto& manager = *manager_result;

  auto result = HandleSet(manager.get(), "logging.level", "debug");
  ASSERT_TRUE(result.has_value());
  EXPECT_THAT(*result, HasSubstr("OK"));
}

TEST_F(HandlerTest, HandleGet_WithManager_GetsVariable) {
  nvecd::config::Config config;
  auto manager_result = nvecd::config::RuntimeVariableManager::Create(config);
  ASSERT_TRUE(manager_result.has_value());
  auto& manager = *manager_result;

  auto result = HandleGet(manager.get(), "logging.level");
  ASSERT_TRUE(result.has_value());
  // Response is a bulk string with the value
  EXPECT_THAT(*result, HasSubstr("$"));
}

TEST_F(HandlerTest, HandleShowVariables_WithManager_ReturnsList) {
  nvecd::config::Config config;
  auto manager_result = nvecd::config::RuntimeVariableManager::Create(config);
  ASSERT_TRUE(manager_result.has_value());
  auto& manager = *manager_result;

  auto result = HandleShowVariables(manager.get());
  ASSERT_TRUE(result.has_value());
  // Response starts with array count
  EXPECT_THAT(*result, HasSubstr("*"));
}

TEST_F(HandlerTest, HandleShowVariables_WithPattern_FiltersResults) {
  nvecd::config::Config config;
  auto manager_result = nvecd::config::RuntimeVariableManager::Create(config);
  ASSERT_TRUE(manager_result.has_value());
  auto& manager = *manager_result;

  auto result = HandleShowVariables(manager.get(), "logging.%");
  ASSERT_TRUE(result.has_value());
  EXPECT_THAT(*result, HasSubstr("logging."));
}

TEST_F(HandlerTest, HandleSet_ImmutableVariable_ReturnsError) {
  nvecd::config::Config config;
  auto manager_result = nvecd::config::RuntimeVariableManager::Create(config);
  ASSERT_TRUE(manager_result.has_value());
  auto& manager = *manager_result;

  // Attempting to set an immutable variable should fail
  auto result = HandleSet(manager.get(), "api.tcp.port", "9999");
  ASSERT_FALSE(result.has_value());
}
