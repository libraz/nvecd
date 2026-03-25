/**
 * @file runtime_variable_manager_test.cpp
 * @brief Unit tests for RuntimeVariableManager
 */

#include "config/runtime_variable_manager.h"

#include <gtest/gtest.h>

#include <string>

using namespace nvecd::config;

/**
 * @brief Helper to create a default Config for testing
 */
static Config MakeDefaultConfig() {
  Config config;
  config.logging.level = "info";
  config.logging.json = true;
  config.logging.file = "/var/log/nvecd.log";
  config.cache.enabled = true;
  config.cache.min_query_cost_ms = 10.0;
  config.cache.ttl_seconds = 3600;
  config.cache.max_memory_bytes = 32 * 1024 * 1024;
  config.cache.compression_enabled = true;
  config.cache.eviction_batch_size = 10;
  config.api.tcp.bind = "127.0.0.1";
  config.api.tcp.port = 11017;
  config.api.http.enable = false;
  config.api.http.bind = "0.0.0.0";
  config.api.http.port = 8080;
  config.events.ctx_buffer_size = 50;
  config.vectors.default_dimension = 768;
  config.vectors.distance_metric = "cosine";
  config.similarity.fusion_alpha = 0.6;
  config.snapshot.dir = "/tmp/snapshots";
  config.perf.thread_pool_size = 8;
  return config;
}

// ============================================================================
// Factory: Create()
// ============================================================================

TEST(RuntimeVariableManagerTest, Create_ReturnsValidManager) {
  Config config = MakeDefaultConfig();
  auto result = RuntimeVariableManager::Create(config);
  ASSERT_TRUE(result) << "Create() failed: " << result.error().message();
  EXPECT_NE(*result, nullptr);
}

// ============================================================================
// GetVariable: known mutable variables
// ============================================================================

TEST(RuntimeVariableManagerTest, GetVariable_LoggingLevel) {
  Config config = MakeDefaultConfig();
  auto manager = *RuntimeVariableManager::Create(config);

  auto val = manager->GetVariable("logging.level");
  ASSERT_TRUE(val) << val.error().message();
  EXPECT_EQ(*val, "info");
}

TEST(RuntimeVariableManagerTest, GetVariable_LoggingJson) {
  Config config = MakeDefaultConfig();
  auto manager = *RuntimeVariableManager::Create(config);

  auto val = manager->GetVariable("logging.json");
  ASSERT_TRUE(val) << val.error().message();
  EXPECT_EQ(*val, "true");
}

TEST(RuntimeVariableManagerTest, GetVariable_CacheEnabled) {
  Config config = MakeDefaultConfig();
  auto manager = *RuntimeVariableManager::Create(config);

  auto val = manager->GetVariable("cache.enabled");
  ASSERT_TRUE(val) << val.error().message();
  EXPECT_EQ(*val, "true");
}

TEST(RuntimeVariableManagerTest, GetVariable_CacheMinQueryCostMs) {
  Config config = MakeDefaultConfig();
  auto manager = *RuntimeVariableManager::Create(config);

  auto val = manager->GetVariable("cache.min_query_cost_ms");
  ASSERT_TRUE(val) << val.error().message();
  // std::to_string(10.0) produces "10.000000"
  EXPECT_EQ(*val, std::to_string(10.0));
}

TEST(RuntimeVariableManagerTest, GetVariable_CacheTtlSeconds) {
  Config config = MakeDefaultConfig();
  auto manager = *RuntimeVariableManager::Create(config);

  auto val = manager->GetVariable("cache.ttl_seconds");
  ASSERT_TRUE(val) << val.error().message();
  EXPECT_EQ(*val, "3600");
}

// ============================================================================
// GetVariable: immutable variables from base config
// ============================================================================

TEST(RuntimeVariableManagerTest, GetVariable_ImmutableApiTcpPort) {
  Config config = MakeDefaultConfig();
  auto manager = *RuntimeVariableManager::Create(config);

  auto val = manager->GetVariable("api.tcp.port");
  ASSERT_TRUE(val) << val.error().message();
  EXPECT_EQ(*val, "11017");
}

TEST(RuntimeVariableManagerTest, GetVariable_ImmutableVectorsDimension) {
  Config config = MakeDefaultConfig();
  auto manager = *RuntimeVariableManager::Create(config);

  auto val = manager->GetVariable("vectors.default_dimension");
  ASSERT_TRUE(val) << val.error().message();
  EXPECT_EQ(*val, "768");
}

// ============================================================================
// GetVariable: unknown variable fails
// ============================================================================

TEST(RuntimeVariableManagerTest, GetVariable_UnknownVariable_ReturnsError) {
  Config config = MakeDefaultConfig();
  auto manager = *RuntimeVariableManager::Create(config);

  auto val = manager->GetVariable("nonexistent.variable");
  EXPECT_FALSE(val);
}

// ============================================================================
// SetVariable: mutable variables succeed
// ============================================================================

TEST(RuntimeVariableManagerTest, SetVariable_LoggingLevel_Debug) {
  Config config = MakeDefaultConfig();
  auto manager = *RuntimeVariableManager::Create(config);

  auto result = manager->SetVariable("logging.level", "debug");
  ASSERT_TRUE(result) << result.error().message();

  auto val = manager->GetVariable("logging.level");
  ASSERT_TRUE(val);
  EXPECT_EQ(*val, "debug");
}

TEST(RuntimeVariableManagerTest, SetVariable_LoggingLevel_AllLevels) {
  Config config = MakeDefaultConfig();
  auto manager = *RuntimeVariableManager::Create(config);

  for (const auto& level : {"trace", "debug", "info", "warn", "error"}) {
    auto result = manager->SetVariable("logging.level", level);
    ASSERT_TRUE(result) << "Failed for level '" << level << "': " << result.error().message();

    auto val = manager->GetVariable("logging.level");
    ASSERT_TRUE(val);
    EXPECT_EQ(*val, level);
  }
}

TEST(RuntimeVariableManagerTest, SetVariable_LoggingLevel_InvalidValue_Fails) {
  Config config = MakeDefaultConfig();
  auto manager = *RuntimeVariableManager::Create(config);

  auto result = manager->SetVariable("logging.level", "verbose");
  EXPECT_FALSE(result);
}

TEST(RuntimeVariableManagerTest, SetVariable_CacheEnabled_Toggle) {
  Config config = MakeDefaultConfig();
  auto manager = *RuntimeVariableManager::Create(config);

  // Disable cache
  auto result = manager->SetVariable("cache.enabled", "false");
  ASSERT_TRUE(result) << result.error().message();

  auto val = manager->GetVariable("cache.enabled");
  ASSERT_TRUE(val);
  EXPECT_EQ(*val, "false");

  // Re-enable cache
  result = manager->SetVariable("cache.enabled", "true");
  ASSERT_TRUE(result) << result.error().message();

  val = manager->GetVariable("cache.enabled");
  ASSERT_TRUE(val);
  EXPECT_EQ(*val, "true");
}

TEST(RuntimeVariableManagerTest, SetVariable_CacheEnabled_InvalidValue_Fails) {
  Config config = MakeDefaultConfig();
  auto manager = *RuntimeVariableManager::Create(config);

  auto result = manager->SetVariable("cache.enabled", "maybe");
  EXPECT_FALSE(result);
}

TEST(RuntimeVariableManagerTest, SetVariable_CacheTtlSeconds) {
  Config config = MakeDefaultConfig();
  auto manager = *RuntimeVariableManager::Create(config);

  auto result = manager->SetVariable("cache.ttl_seconds", "7200");
  ASSERT_TRUE(result) << result.error().message();

  auto val = manager->GetVariable("cache.ttl_seconds");
  ASSERT_TRUE(val);
  EXPECT_EQ(*val, "7200");
}

TEST(RuntimeVariableManagerTest, SetVariable_CacheTtlSeconds_Negative_Fails) {
  Config config = MakeDefaultConfig();
  auto manager = *RuntimeVariableManager::Create(config);

  auto result = manager->SetVariable("cache.ttl_seconds", "-1");
  EXPECT_FALSE(result);
}

TEST(RuntimeVariableManagerTest, SetVariable_CacheTtlSeconds_InvalidInt_Fails) {
  Config config = MakeDefaultConfig();
  auto manager = *RuntimeVariableManager::Create(config);

  auto result = manager->SetVariable("cache.ttl_seconds", "abc");
  EXPECT_FALSE(result);
}

TEST(RuntimeVariableManagerTest, SetVariable_CacheMinQueryCostMs) {
  Config config = MakeDefaultConfig();
  auto manager = *RuntimeVariableManager::Create(config);

  auto result = manager->SetVariable("cache.min_query_cost_ms", "5.5");
  ASSERT_TRUE(result) << result.error().message();

  auto val = manager->GetVariable("cache.min_query_cost_ms");
  ASSERT_TRUE(val);
  EXPECT_EQ(*val, "5.5");
}

TEST(RuntimeVariableManagerTest, SetVariable_CacheMinQueryCostMs_Negative_Fails) {
  Config config = MakeDefaultConfig();
  auto manager = *RuntimeVariableManager::Create(config);

  auto result = manager->SetVariable("cache.min_query_cost_ms", "-1.0");
  EXPECT_FALSE(result);
}

TEST(RuntimeVariableManagerTest, SetVariable_LoggingJson_Toggle) {
  Config config = MakeDefaultConfig();
  auto manager = *RuntimeVariableManager::Create(config);

  auto result = manager->SetVariable("logging.json", "false");
  ASSERT_TRUE(result) << result.error().message();

  auto val = manager->GetVariable("logging.json");
  ASSERT_TRUE(val);
  EXPECT_EQ(*val, "false");
}

// ============================================================================
// SetVariable: immutable variables fail
// ============================================================================

TEST(RuntimeVariableManagerTest, SetVariable_ImmutableApiTcpPort_Fails) {
  Config config = MakeDefaultConfig();
  auto manager = *RuntimeVariableManager::Create(config);

  auto result = manager->SetVariable("api.tcp.port", "9999");
  EXPECT_FALSE(result);
}

TEST(RuntimeVariableManagerTest, SetVariable_ImmutableVectorsDimension_Fails) {
  Config config = MakeDefaultConfig();
  auto manager = *RuntimeVariableManager::Create(config);

  auto result = manager->SetVariable("vectors.default_dimension", "512");
  EXPECT_FALSE(result);
}

TEST(RuntimeVariableManagerTest, SetVariable_ImmutableCacheMaxMemory_Fails) {
  Config config = MakeDefaultConfig();
  auto manager = *RuntimeVariableManager::Create(config);

  auto result = manager->SetVariable("cache.max_memory_bytes", "1024");
  EXPECT_FALSE(result);
}

TEST(RuntimeVariableManagerTest, SetVariable_ImmutableSnapshotDir_Fails) {
  Config config = MakeDefaultConfig();
  auto manager = *RuntimeVariableManager::Create(config);

  auto result = manager->SetVariable("snapshot.dir", "/new/path");
  EXPECT_FALSE(result);
}

// ============================================================================
// SetVariable: unknown variables fail
// ============================================================================

TEST(RuntimeVariableManagerTest, SetVariable_UnknownVariable_Fails) {
  Config config = MakeDefaultConfig();
  auto manager = *RuntimeVariableManager::Create(config);

  auto result = manager->SetVariable("nonexistent.variable", "value");
  EXPECT_FALSE(result);
}

// ============================================================================
// IsMutable
// ============================================================================

TEST(RuntimeVariableManagerTest, IsMutable_MutableVariables) {
  EXPECT_TRUE(RuntimeVariableManager::IsMutable("logging.level"));
  EXPECT_TRUE(RuntimeVariableManager::IsMutable("logging.json"));
  EXPECT_TRUE(RuntimeVariableManager::IsMutable("cache.enabled"));
  EXPECT_TRUE(RuntimeVariableManager::IsMutable("cache.min_query_cost_ms"));
  EXPECT_TRUE(RuntimeVariableManager::IsMutable("cache.ttl_seconds"));
}

TEST(RuntimeVariableManagerTest, IsMutable_ImmutableVariables) {
  EXPECT_FALSE(RuntimeVariableManager::IsMutable("api.tcp.port"));
  EXPECT_FALSE(RuntimeVariableManager::IsMutable("api.tcp.bind"));
  EXPECT_FALSE(RuntimeVariableManager::IsMutable("vectors.default_dimension"));
  EXPECT_FALSE(RuntimeVariableManager::IsMutable("cache.max_memory_bytes"));
  EXPECT_FALSE(RuntimeVariableManager::IsMutable("snapshot.dir"));
  EXPECT_FALSE(RuntimeVariableManager::IsMutable("perf.thread_pool_size"));
}

TEST(RuntimeVariableManagerTest, IsMutable_UnknownVariable) {
  EXPECT_FALSE(RuntimeVariableManager::IsMutable("nonexistent.variable"));
}

// ============================================================================
// GetAllVariables
// ============================================================================

TEST(RuntimeVariableManagerTest, GetAllVariables_ReturnsAllKnownVariables) {
  Config config = MakeDefaultConfig();
  auto manager = *RuntimeVariableManager::Create(config);

  auto all_vars = manager->GetAllVariables();
  // Should contain both mutable and immutable variables
  EXPECT_FALSE(all_vars.empty());

  // Check a few known variables are present
  EXPECT_NE(all_vars.find("logging.level"), all_vars.end());
  EXPECT_NE(all_vars.find("cache.enabled"), all_vars.end());
  EXPECT_NE(all_vars.find("api.tcp.port"), all_vars.end());
  EXPECT_NE(all_vars.find("vectors.default_dimension"), all_vars.end());
}

TEST(RuntimeVariableManagerTest, GetAllVariables_MutabilityFlagCorrect) {
  Config config = MakeDefaultConfig();
  auto manager = *RuntimeVariableManager::Create(config);

  auto all_vars = manager->GetAllVariables();

  // Mutable variables should have mutable_ == true
  EXPECT_TRUE(all_vars.at("logging.level").mutable_);
  EXPECT_TRUE(all_vars.at("cache.enabled").mutable_);

  // Immutable variables should have mutable_ == false
  EXPECT_FALSE(all_vars.at("api.tcp.port").mutable_);
  EXPECT_FALSE(all_vars.at("vectors.default_dimension").mutable_);
}

TEST(RuntimeVariableManagerTest, GetAllVariables_PrefixFilter_Logging) {
  Config config = MakeDefaultConfig();
  auto manager = *RuntimeVariableManager::Create(config);

  auto logging_vars = manager->GetAllVariables("logging");
  EXPECT_FALSE(logging_vars.empty());

  for (const auto& [name, info] : logging_vars) {
    EXPECT_EQ(name.find("logging"), 0U) << "Variable '" << name << "' does not start with 'logging'";
  }
}

TEST(RuntimeVariableManagerTest, GetAllVariables_PrefixFilter_Cache) {
  Config config = MakeDefaultConfig();
  auto manager = *RuntimeVariableManager::Create(config);

  auto cache_vars = manager->GetAllVariables("cache");
  EXPECT_FALSE(cache_vars.empty());

  for (const auto& [name, info] : cache_vars) {
    EXPECT_EQ(name.find("cache"), 0U) << "Variable '" << name << "' does not start with 'cache'";
  }
}

TEST(RuntimeVariableManagerTest, GetAllVariables_PrefixFilter_NoMatch) {
  Config config = MakeDefaultConfig();
  auto manager = *RuntimeVariableManager::Create(config);

  auto vars = manager->GetAllVariables("zzz_nonexistent");
  EXPECT_TRUE(vars.empty());
}

// ============================================================================
// SetVariable updates reflected in GetAllVariables
// ============================================================================

TEST(RuntimeVariableManagerTest, SetVariable_ReflectedInGetAllVariables) {
  Config config = MakeDefaultConfig();
  auto manager = *RuntimeVariableManager::Create(config);

  auto set_result = manager->SetVariable("logging.level", "debug");
  ASSERT_TRUE(set_result) << set_result.error().message();

  auto all_vars = manager->GetAllVariables();
  EXPECT_EQ(all_vars.at("logging.level").value, "debug");
}

// ============================================================================
// CacheToggleCallback integration
// ============================================================================

TEST(RuntimeVariableManagerTest, SetCacheToggleCallback_CalledOnToggle) {
  Config config = MakeDefaultConfig();
  auto manager = *RuntimeVariableManager::Create(config);

  bool callback_called = false;
  bool callback_value = false;
  manager->SetCacheToggleCallback([&](bool enabled) -> nvecd::utils::Expected<void, nvecd::utils::Error> {
    callback_called = true;
    callback_value = enabled;
    return {};
  });

  auto result = manager->SetVariable("cache.enabled", "false");
  ASSERT_TRUE(result) << result.error().message();
  EXPECT_TRUE(callback_called);
  EXPECT_FALSE(callback_value);
}
