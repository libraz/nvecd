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
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>

#include "cache/cache_key.h"
#include "cache/similarity_cache.h"
#include "cache/similarity_cache_controller.h"
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
#include "storage/snapshot_format_v1.h"
#include "storage/wal.h"
#include "storage/wal_checkpoint.h"
#include "utils/error.h"
#include "vectors/metadata_store.h"
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
    dump_dir_ = std::filesystem::temp_directory_path() / ("nvecd_handler_test_" + std::to_string(::getpid()));
    std::filesystem::remove_all(dump_dir_);
    std::filesystem::create_directories(dump_dir_);
    std::filesystem::permissions(dump_dir_, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace);

    // Create real objects for all handler dependencies
    config_ = std::make_unique<nvecd::config::Config>();
    config_->cache.enabled = true;

    nvecd::config::EventsConfig events_cfg;
    event_store_ = std::make_unique<nvecd::events::EventStore>(events_cfg);

    co_index_ = std::make_unique<nvecd::events::CoOccurrenceIndex>();

    nvecd::config::VectorsConfig vectors_cfg;
    vector_store_ = std::make_unique<nvecd::vectors::VectorStore>(vectors_cfg);
    metadata_store_ = std::make_unique<nvecd::vectors::MetadataStore>();

    nvecd::config::SimilarityConfig sim_cfg;
    similarity_engine_ = std::make_unique<nvecd::similarity::SimilarityEngine>(
        event_store_.get(), co_index_.get(), vector_store_.get(), sim_cfg, vectors_cfg);

    auto manager_result = nvecd::config::RuntimeVariableManager::Create(*config_);
    ASSERT_TRUE(manager_result.has_value()) << "Failed to create RuntimeVariableManager";
    variable_manager_ = std::move(*manager_result);

    // Construct HandlerContext in-place (non-copyable, non-movable due to atomics)
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
                                             /*.dump_dir=*/dump_dir_.string(),
                                             /*.requirepass=*/""};
    cache_controller_ =
        std::make_unique<nvecd::cache::SimilarityCacheController>(1024 * 1024, 0, 0, true, 1, true, &ctx_->cache);
    cache_ = &cache_controller_->Cache();
    variable_manager_->SetCacheController(cache_controller_.get());
    ctx_->cache_controller = cache_controller_.get();
    ctx_->snapshot_write_gate = &snapshot_write_gate_;
    ctx_->write_serialization_gate = &write_serialization_gate_;
  }

  void TearDown() override {
    cache_controller_.reset();
    if (ctx_ != nullptr) {
      ctx_->~HandlerContext();
      ctx_ = nullptr;
    }
    std::filesystem::remove_all(dump_dir_);
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
  nvecd::cache::SimilarityCache* cache_ = nullptr;
  std::unique_ptr<nvecd::cache::SimilarityCacheController> cache_controller_;
  std::unique_ptr<nvecd::config::RuntimeVariableManager> variable_manager_;
  std::filesystem::path dump_dir_;
  std::shared_mutex snapshot_write_gate_;
  std::mutex write_serialization_gate_;

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
                                             /*.metadata_store=*/nullptr,
                                             /*.similarity_engine=*/nullptr,
                                             /*.cache=*/{},
                                             /*.variable_manager=*/nullptr,
                                             /*.stats=*/stats_,
                                             /*.config=*/nullptr,
                                             /*.loading=*/loading_,
                                             /*.read_only=*/read_only_,
                                             /*.dump_dir=*/"",
                                             /*.requirepass=*/""};
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
  ASSERT_TRUE(variable_manager_->SetVariable("cache.enabled", "false"));

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
  EXPECT_EQ(ctx_->cache.load(), nullptr);
  EXPECT_EQ(*variable_manager_->GetVariable("cache.enabled"), "false");

  ASSERT_TRUE(variable_manager_->SetVariable("cache.ttl_seconds", "23"));
  ASSERT_TRUE(variable_manager_->SetVariable("cache.min_query_cost_ms", "1.75"));
  EXPECT_EQ(cache_->GetTtl(), 23);
  EXPECT_DOUBLE_EQ(cache_->GetMinQueryCost(), 1.75);

  result = HandleCacheEnable(*ctx_);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(ctx_->cache.load(), cache_);
  EXPECT_EQ(*variable_manager_->GetVariable("cache.enabled"), "true");

  auto stats_result = HandleCacheStats(*ctx_);
  ASSERT_TRUE(stats_result);
  EXPECT_THAT(*stats_result, HasSubstr("ttl_seconds: 23"));
  EXPECT_THAT(*stats_result, HasSubstr("min_query_cost_ms: 1.75"));
}

// ============================================================================
// Cache Handler Null Tests
// ============================================================================

TEST_F(HandlerNullTest, CacheStats_NullCache_ReturnsDisabled) {
  auto result = HandleCacheStats(*ctx_);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), nvecd::utils::ErrorCode::kInternalError);
}

TEST_F(HandlerNullTest, CacheClear_NullCache_ReturnsNoCache) {
  auto result = HandleCacheClear(*ctx_);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), nvecd::utils::ErrorCode::kInternalError);
}

TEST_F(HandlerNullTest, CacheEnable_NullCache_ReturnsNoInstance) {
  auto result = HandleCacheEnable(*ctx_);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), nvecd::utils::ErrorCode::kInternalError);
}

TEST_F(HandlerNullTest, CacheDisable_NullCache_ReturnsNoInstance) {
  auto result = HandleCacheDisable(*ctx_);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), nvecd::utils::ErrorCode::kInternalError);
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

TEST_F(HandlerTest, Info_GoldenEnvelopeMatchesMygramOracle) {
  auto result = HandleInfo(*ctx_);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->find("OK INFO"), 0U);
  ASSERT_GE(result->size(), 5U);
  EXPECT_EQ(result->substr(result->size() - 5), "END\r\n");
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
  EXPECT_EQ(*result, "OK DEBUG_ON\r\n");
  EXPECT_TRUE(conn_ctx.debug_mode);
}

TEST_F(HandlerTest, DebugOff_SetsDebugModeFalse) {
  ConnectionContext conn_ctx;
  conn_ctx.debug_mode = true;

  auto result = HandleDebugOff(conn_ctx);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, "OK DEBUG_OFF\r\n");
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

TEST_F(HandlerTest, ConfigShow_GoldenEnvelopeMatchesMygramOracle) {
  ServerContext server_context;
  server_context.config = config_.get();
  auto result = HandleConfigShow(server_context, "");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->find("+OK"), 0U);
  ASSERT_GE(result->size(), 5U);
  EXPECT_EQ(result->substr(result->size() - 5), "END\r\n");
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

TEST_F(HandlerTest, DumpStatus_GoldenEnvelopeMatchesMygramOracle) {
  auto result = HandleDumpStatus(*ctx_);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, "OK DUMP_STATUS\r\nstatus: idle\r\nEND\r\n");
}

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

TEST_F(HandlerTest, DumpSave_WithValidData_Succeeds) {
  // Populate vector data
  vector_store_->SetVector("item1", {0.1f, 0.2f, 0.3f});

  // Use lock mode since fork_snapshot_writer is not available in test
  config_->snapshot.mode = "lock";
  config_->snapshot.dir = dump_dir_.string();

  const std::string dump_path = (dump_dir_ / "snapshot.dmp").string();
  auto result = HandleDumpSave(*ctx_, dump_path);
  ASSERT_TRUE(result.has_value()) << "DumpSave failed: " << (result.has_value() ? "" : result.error().message());
  EXPECT_THAT(*result, HasSubstr("DUMP_SAVED"));

  // Verify file was created
  EXPECT_TRUE(std::filesystem::exists(dump_path));

  // Clean up
  std::filesystem::remove(dump_path);
}

TEST_F(HandlerTest, DumpLoad_RoundTrip_RestoresData) {
  // Populate data
  vector_store_->SetVector("item1", {0.1f, 0.2f, 0.3f});
  vector_store_->SetVector("item2", {0.4f, 0.5f, 0.6f});

  config_->snapshot.mode = "lock";
  config_->snapshot.dir = dump_dir_.string();

  const std::string dump_path = (dump_dir_ / "roundtrip.dmp").string();

  // Save
  auto save_result = HandleDumpSave(*ctx_, dump_path);
  ASSERT_TRUE(save_result.has_value()) << "DumpSave failed: " << save_result.error().message();

  // Clear stores
  vector_store_->Clear();
  ASSERT_EQ(vector_store_->GetVectorCount(), 0u);

  // Load
  auto load_result = HandleDumpLoad(*ctx_, dump_path);
  ASSERT_TRUE(load_result.has_value()) << "DumpLoad failed: " << load_result.error().message();
  EXPECT_THAT(*load_result, HasSubstr("DUMP_LOADED"));

  // Verify data is restored
  EXPECT_GE(vector_store_->GetVectorCount(), 2u);
  auto vec1 = vector_store_->GetVector("item1");
  EXPECT_TRUE(vec1.has_value()) << "item1 should be restored after load";

  // Clean up
  std::filesystem::remove(dump_path);
}

TEST_F(HandlerTest, DumpLoad_IsSingleFlightAndTakesExclusiveSnapshotGate) {
  vector_store_->SetVector("item1", {0.1f, 0.2f, 0.3f});
  config_->snapshot.mode = "lock";
  config_->snapshot.dir = dump_dir_.string();
  const std::string dump_path = (dump_dir_ / "single_flight.dmp").string();
  ASSERT_TRUE(HandleDumpSave(*ctx_, dump_path).has_value());

  std::shared_lock<std::shared_mutex> active_query(snapshot_write_gate_);
  auto first_load = std::async(std::launch::async, [this, &dump_path] { return HandleDumpLoad(*ctx_, dump_path); });

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
  while (!loading_.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }
  ASSERT_TRUE(loading_.load(std::memory_order_acquire));
  EXPECT_EQ(first_load.wait_for(std::chrono::milliseconds(20)), std::future_status::timeout);

  auto second_load = HandleDumpLoad(*ctx_, dump_path);
  ASSERT_FALSE(second_load.has_value());
  EXPECT_EQ(second_load.error().code(), nvecd::utils::ErrorCode::kSnapshotAlreadyInProgress);

  active_query.unlock();
  ASSERT_TRUE(first_load.get().has_value());
  EXPECT_FALSE(loading_.load(std::memory_order_acquire));
}

TEST_F(HandlerTest, DumpLoad_InvalidatesCacheAndAdvancesVectorGeneration) {
  vector_store_->SetVector("item1", {0.1f, 0.2f, 0.3f});
  config_->snapshot.mode = "lock";
  config_->snapshot.dir = dump_dir_.string();
  const std::string dump_path = (dump_dir_ / "cache_generation.dmp").string();
  ASSERT_TRUE(HandleDumpSave(*ctx_, dump_path).has_value());

  ASSERT_TRUE(cache_->Insert(nvecd::cache::CacheKey{1, 2}, {{"item1", 1.0F}}, 1.0));
  ASSERT_EQ(cache_->GetStatistics().current_entries, 1U);
  ctx_->vector_generation.store(7, std::memory_order_release);
  ctx_->metadata_generation.store(9, std::memory_order_release);
  ctx_->dataset_generation.store(11, std::memory_order_release);

  ASSERT_TRUE(HandleDumpLoad(*ctx_, dump_path).has_value());
  EXPECT_EQ(ctx_->vector_generation.load(std::memory_order_acquire), 8U);
  EXPECT_EQ(ctx_->metadata_generation.load(std::memory_order_acquire), 10U);
  EXPECT_EQ(ctx_->dataset_generation.load(std::memory_order_acquire), 12U);
  EXPECT_EQ(cache_->GetStatistics().current_entries, 0U);
}

TEST_F(HandlerTest, DumpLoadFailureLeavesLiveStateAndCacheUnchanged) {
  ASSERT_TRUE(vector_store_->SetVector("live-item", {0.1f, 0.2f, 0.3f}).has_value());
  ASSERT_TRUE(cache_->Insert(nvecd::cache::CacheKey{1, 2}, {{"live-item", 1.0F}}, 1.0));
  ctx_->vector_generation.store(11, std::memory_order_release);
  ctx_->metadata_generation.store(12, std::memory_order_release);
  ctx_->dataset_generation.store(13, std::memory_order_release);

  auto result = HandleDumpLoad(*ctx_, (dump_dir_ / "missing.dmp").string());

  ASSERT_FALSE(result.has_value());
  EXPECT_TRUE(vector_store_->HasVector("live-item"));
  EXPECT_EQ(ctx_->vector_generation.load(std::memory_order_acquire), 11U);
  EXPECT_EQ(ctx_->metadata_generation.load(std::memory_order_acquire), 12U);
  EXPECT_EQ(ctx_->dataset_generation.load(std::memory_order_acquire), 13U);
  EXPECT_EQ(cache_->GetStatistics().current_entries, 1U);
}

TEST_F(HandlerTest, DumpLoadSemanticFailureIsAtomic) {
  ASSERT_TRUE(vector_store_->SetVector("live-item", {0.1f, 0.2f, 0.3f}).has_value());
  metadata_store_->Set("live-item", {{"state", std::string("live")}});
  ASSERT_TRUE(cache_->Insert(nvecd::cache::CacheKey{1, 2}, {{"live-item", 1.0F}}, 1.0));
  ctx_->vector_generation.store(13, std::memory_order_release);

  nvecd::events::EventStore snapshot_events(config_->events);
  nvecd::events::CoOccurrenceIndex snapshot_co;
  nvecd::vectors::VectorStore snapshot_vectors(config_->vectors);
  nvecd::vectors::MetadataStore invalid_metadata;
  ASSERT_TRUE(snapshot_vectors.SetVector("snapshot-item", {0.4f, 0.5f, 0.6f}).has_value());
  invalid_metadata.Set("orphan-item", {{"state", std::string("invalid")}});
  const std::string dump_path = (dump_dir_ / "semantic_invalid.dmp").string();
  ASSERT_TRUE(nvecd::storage::snapshot_v1::WriteSnapshotV1(dump_path, *config_, snapshot_events, snapshot_co,
                                                           snapshot_vectors, nullptr, nullptr, &invalid_metadata)
                  .has_value());

  auto result = HandleDumpLoad(*ctx_, dump_path);

  ASSERT_FALSE(result.has_value());
  EXPECT_TRUE(vector_store_->HasVector("live-item"));
  EXPECT_FALSE(vector_store_->HasVector("snapshot-item"));
  ASSERT_NE(metadata_store_->Get("live-item"), nullptr);
  EXPECT_EQ(ctx_->vector_generation.load(std::memory_order_acquire), 13U);
  EXPECT_EQ(cache_->GetStatistics().current_entries, 1U);
}

TEST_F(HandlerTest, DumpLoadWithWalMakesLoadedSnapshotTheRecoveryBase) {
  const std::string root = ::testing::TempDir() + "/nvecd_dump_load_wal";
  const std::string dump_path = root + "/rollback.dmp";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);

  nvecd::storage::WriteAheadLog wal;
  nvecd::storage::WriteAheadLog::Config wal_config;
  wal_config.directory = root + "/wal";
  ASSERT_TRUE(wal.Open(wal_config).has_value());
  ctx_->wal = &wal;
  config_->snapshot.mode = "lock";
  config_->snapshot.dir = root;
  ctx_->dump_dir = root;
  vector_store_->SetVector("before", {0.1F, 0.2F, 0.3F});
  ASSERT_TRUE(HandleDumpSave(*ctx_, dump_path).has_value());

  // Simulate a durable mutation after the snapshot. A rollback load must move
  // the checkpoint past this record so a later restart does not replay it.
  const uint8_t payload = 0;
  ASSERT_TRUE(wal.Append(nvecd::storage::WalOpType::kEventAdd, &payload, 1).has_value());
  const uint64_t sequence_before_load = wal.CurrentSequence();

  vector_store_->Clear();
  ASSERT_TRUE(HandleDumpLoad(*ctx_, dump_path).has_value());
  auto checkpoint = nvecd::storage::ReadWalCheckpoint(dump_path);
  ASSERT_TRUE(checkpoint.has_value()) << checkpoint.error().message();
  EXPECT_EQ(*checkpoint, sequence_before_load);
  EXPECT_TRUE(vector_store_->HasVector("before"));

  wal.Close();
  std::filesystem::remove_all(root);
}

TEST_F(HandlerTest, DumpLoadDurabilityFailureEntersFailStop) {
  const std::string dump_path = (dump_dir_ / "fail_stop.dmp").string();
  nvecd::storage::WriteAheadLog wal;
  nvecd::storage::WriteAheadLog::Config wal_config;
  wal_config.directory = (dump_dir_ / "wal").string();
  ASSERT_TRUE(wal.Open(wal_config).has_value());
  ctx_->wal = &wal;
  config_->snapshot.mode = "lock";
  config_->snapshot.dir = dump_dir_.string();
  ASSERT_TRUE(vector_store_->SetVector("snapshot-item", {0.1F, 0.2F, 0.3F}).has_value());
  ASSERT_TRUE(HandleDumpSave(*ctx_, dump_path).has_value());

  vector_store_->Clear();
  ASSERT_TRUE(vector_store_->SetVector("live-item", {0.4F, 0.5F, 0.6F}).has_value());
  // Force the checkpoint's atomic rename to fail independently of effective
  // UID. chmod-based failure injection is bypassed when Linux containers run
  // as root. A directory at the sidecar destination cannot be replaced by a
  // regular file through rename(2), even for root.
  const auto checkpoint_path = std::filesystem::path(dump_path + nvecd::storage::kWalCheckpointSuffix);
  std::filesystem::remove(checkpoint_path);
  std::filesystem::create_directory(checkpoint_path);
  auto result = HandleDumpLoad(*ctx_, dump_path);

  ASSERT_FALSE(result.has_value());
  EXPECT_TRUE(loading_.load(std::memory_order_acquire));
  EXPECT_TRUE(read_only_.load(std::memory_order_acquire));
  EXPECT_TRUE(vector_store_->HasVector("snapshot-item"));
  EXPECT_FALSE(vector_store_->HasVector("live-item"));
  wal.Close();
}

TEST_F(HandlerTest, DumpVerify_ValidFile_Succeeds) {
  // Populate and save data first
  vector_store_->SetVector("item1", {0.1f, 0.2f, 0.3f});

  config_->snapshot.mode = "lock";
  config_->snapshot.dir = dump_dir_.string();

  const std::string dump_path = (dump_dir_ / "verify.dmp").string();
  auto save_result = HandleDumpSave(*ctx_, dump_path);
  ASSERT_TRUE(save_result.has_value()) << "DumpSave failed: " << save_result.error().message();

  // Verify the saved file
  auto verify_result = HandleDumpVerify(dump_dir_.string(), dump_path);
  ASSERT_TRUE(verify_result.has_value()) << "DumpVerify failed: " << verify_result.error().message();
  EXPECT_THAT(*verify_result, HasSubstr("DUMP_VERIFIED"));

  // Clean up
  std::filesystem::remove(dump_path);
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
