/**
 * @file config_test.cpp
 * @brief Unit tests for configuration parser
 */

#include "config/config.h"

#include <gtest/gtest.h>

#include <fstream>
#include <limits>

#include "config/config_help.h"

using namespace nvecd::config;

/**
 * @brief Test loading valid configuration file
 */
TEST(ConfigTest, LoadValidConfig) {
  auto config_result = LoadConfig("test_config.yaml");
  ASSERT_TRUE(config_result) << "Failed to load config: " << config_result.error().message();
  Config config = *config_result;

  // Events config
  EXPECT_EQ(config.events.ctx_buffer_size, 100);
  EXPECT_EQ(config.events.decay_interval_sec, 1800);
  EXPECT_DOUBLE_EQ(config.events.decay_alpha, 0.95);

  // Vectors config
  EXPECT_EQ(config.vectors.default_dimension, 384);
  EXPECT_EQ(config.vectors.distance_metric, "dot");

  // Similarity config
  EXPECT_EQ(config.similarity.default_top_k, 50);
  EXPECT_EQ(config.similarity.max_top_k, 500);
  EXPECT_DOUBLE_EQ(config.similarity.fusion_alpha, 0.7);
  EXPECT_DOUBLE_EQ(config.similarity.fusion_beta, 0.3);

  // Snapshot config
  EXPECT_EQ(config.snapshot.dir, "/tmp/nvecd_test_snapshots");
  EXPECT_EQ(config.snapshot.default_filename, "test.snapshot");
  EXPECT_EQ(config.snapshot.interval_sec, 600);
  EXPECT_EQ(config.snapshot.retain, 5);
  EXPECT_EQ(config.snapshot.mode, "fork");

  // Performance config
  EXPECT_EQ(config.perf.thread_pool_size, 4);
  EXPECT_EQ(config.perf.max_connections, 500);
  EXPECT_EQ(config.perf.connection_timeout_sec, 60);

  // API config
  EXPECT_EQ(config.api.tcp.bind, "127.0.0.1");
  EXPECT_EQ(config.api.tcp.port, 12345);
  EXPECT_TRUE(config.api.http.enable);
  EXPECT_EQ(config.api.http.bind, "0.0.0.0");
  EXPECT_EQ(config.api.http.port, 9090);
  EXPECT_TRUE(config.api.http.enable_cors);
  EXPECT_EQ(config.api.http.cors_allow_origin, "https://example.com");

  // Rate limiting
  EXPECT_TRUE(config.api.rate_limiting.enable);
  EXPECT_EQ(config.api.rate_limiting.capacity, 50);
  EXPECT_EQ(config.api.rate_limiting.refill_rate, 5);
  EXPECT_EQ(config.api.rate_limiting.max_clients, 1000);

  // Network config
  ASSERT_EQ(config.network.allow_cidrs.size(), 2);
  EXPECT_EQ(config.network.allow_cidrs[0], "127.0.0.1/32");
  EXPECT_EQ(config.network.allow_cidrs[1], "192.168.1.0/24");

  // Logging config
  EXPECT_EQ(config.logging.level, "debug");
  EXPECT_FALSE(config.logging.json);
  EXPECT_EQ(config.logging.file, "/tmp/nvecd_test.log");

  // Cache config
  EXPECT_FALSE(config.cache.enabled);
  EXPECT_EQ(config.cache.max_memory_bytes, 16 * 1024 * 1024);  // 16 MB in bytes
  EXPECT_DOUBLE_EQ(config.cache.min_query_cost_ms, 5.0);
  EXPECT_EQ(config.cache.ttl_seconds, 1800);
  EXPECT_FALSE(config.cache.compression_enabled);
  EXPECT_EQ(config.cache.eviction_batch_size, 5);
}

/**
 * @brief Test loading non-existent configuration file
 */
TEST(ConfigTest, LoadNonExistentFile) {
  auto config_result = LoadConfig("nonexistent_config.yaml");
  EXPECT_FALSE(config_result);
  EXPECT_EQ(config_result.error().code(), nvecd::utils::ErrorCode::kConfigFileNotFound);
}

/**
 * @brief Test configuration validation with invalid values
 */
TEST(ConfigTest, ValidateInvalidConfig) {
  Config config;

  // Invalid: ctx_buffer_size = 0
  config.events.ctx_buffer_size = 0;
  auto result = ValidateConfig(config);
  EXPECT_FALSE(result);
  EXPECT_EQ(result.error().code(), nvecd::utils::ErrorCode::kConfigInvalidValue);

  // Fix and test invalid decay_alpha
  config.events.ctx_buffer_size = 50;
  config.events.decay_alpha = 1.5;  // > 1.0
  result = ValidateConfig(config);
  EXPECT_FALSE(result);
  EXPECT_EQ(result.error().code(), nvecd::utils::ErrorCode::kConfigInvalidValue);

  // Fix and test invalid distance metric
  config.events.decay_alpha = 0.99;
  config.vectors.distance_metric = "invalid_metric";
  result = ValidateConfig(config);
  EXPECT_FALSE(result);
  EXPECT_EQ(result.error().code(), nvecd::utils::ErrorCode::kConfigInvalidValue);

  // Fix and test invalid port
  config.vectors.distance_metric = "cosine";
  config.api.tcp.port = 99999;  // > 65535
  result = ValidateConfig(config);
  EXPECT_FALSE(result);
  EXPECT_EQ(result.error().code(), nvecd::utils::ErrorCode::kConfigInvalidValue);
}

/**
 * @brief Test valid configuration passes validation
 */
TEST(ConfigTest, ValidateValidConfig) {
  Config config;
  // Use default values from config.h

  auto result = ValidateConfig(config);
  EXPECT_TRUE(result) << "Validation failed: " << result.error().message();
}

TEST(ConfigTest, ValidateConfigRejectsNonFiniteNumericValues) {
  Config config;
  config.events.decay_alpha = std::numeric_limits<double>::quiet_NaN();
  EXPECT_FALSE(ValidateConfig(config));

  config = Config{};
  config.cache.min_query_cost_ms = std::numeric_limits<double>::infinity();
  EXPECT_FALSE(ValidateConfig(config));
}

/**
 * @brief Test loading configuration with minimal settings
 */
TEST(ConfigTest, LoadMinimalConfig) {
  // Create minimal config file
  const char* minimal_config = R"(
events:
  ctx_buffer_size: 10

vectors:
  default_dimension: 128
)";

  std::ofstream ofs("minimal_test_config.yaml");
  ofs << minimal_config;
  ofs.close();

  auto config_result = LoadConfig("minimal_test_config.yaml");
  ASSERT_TRUE(config_result) << "Failed to load minimal config: " << config_result.error().message();

  Config config = *config_result;
  // Check specified values
  EXPECT_EQ(config.events.ctx_buffer_size, 10);
  EXPECT_EQ(config.vectors.default_dimension, 128);

  // Check defaults are used for unspecified values
  EXPECT_EQ(config.events.decay_interval_sec, defaults::kDecayIntervalSec);
  EXPECT_DOUBLE_EQ(config.events.decay_alpha, defaults::kDecayAlpha);
  EXPECT_EQ(config.similarity.default_top_k, defaults::kDefaultTopK);

  // Cleanup
  std::remove("minimal_test_config.yaml");
}

/**
 * @brief Test that all documented flagship-feature keys pass JSON Schema validation
 *
 * Regression for the schema/parser drift where adaptive fusion, temporal
 * co-occurrence, negative signals, index_type and HNSW/IVF tuning keys were
 * read by the parser and documented in the README, but absent from
 * config-schema.json whose sections use additionalProperties:false. A config
 * enabling any of those documented features was rejected at startup.
 */
TEST(ConfigTest, LoadFlagshipFeatureConfig) {
  const char* flagship_config = R"(
events:
  max_contexts: 1000
  max_neighbors_per_item: 25
  min_support: 3.5
  temporal_cooccurrence: true
  temporal_half_life_sec: 43200.0
  negative_signals: true
  negative_weight: 0.3

similarity:
  adaptive_fusion: true
  adaptive_min_alpha: 0.2
  adaptive_max_alpha: 0.9
  adaptive_maturity_threshold: 40
  index_type: hnsw
  hnsw_m: 32
  hnsw_ef_construction: 256
  hnsw_ef_search: 64
  hnsw_max_elements: 100000
  ivf_seal_threshold: 50000
)";

  std::ofstream ofs("flagship_test_config.yaml");
  ofs << flagship_config;
  ofs.close();

  auto config_result = LoadConfig("flagship_test_config.yaml");
  ASSERT_TRUE(config_result) << "Flagship config rejected by schema: " << config_result.error().message();

  Config config = *config_result;
  EXPECT_TRUE(config.events.temporal_cooccurrence);
  EXPECT_EQ(config.events.max_contexts, 1000U);
  EXPECT_EQ(config.events.max_neighbors_per_item, 25U);
  EXPECT_DOUBLE_EQ(config.events.min_support, 3.5);
  EXPECT_DOUBLE_EQ(config.events.temporal_half_life_sec, 43200.0);
  EXPECT_TRUE(config.events.negative_signals);
  EXPECT_DOUBLE_EQ(config.events.negative_weight, 0.3);
  EXPECT_TRUE(config.similarity.adaptive_fusion);
  EXPECT_DOUBLE_EQ(config.similarity.adaptive_min_alpha, 0.2);
  EXPECT_DOUBLE_EQ(config.similarity.adaptive_max_alpha, 0.9);
  EXPECT_EQ(config.similarity.adaptive_maturity_threshold, 40U);
  EXPECT_EQ(config.similarity.index_type, "hnsw");
  EXPECT_EQ(config.similarity.hnsw_m, 32U);
  EXPECT_EQ(config.similarity.hnsw_ef_construction, 256U);
  EXPECT_EQ(config.similarity.hnsw_ef_search, 64U);
  EXPECT_EQ(config.similarity.hnsw_max_elements, 100000U);
  EXPECT_EQ(config.similarity.ivf_seal_threshold, 50000U);

  std::remove("flagship_test_config.yaml");
}

TEST(ConfigTest, DisplayConfigIncludesEmptyAndSensitiveRuntimeVariables) {
  Config config;
  config.cache.max_memory_bytes = 32 * 1024 * 1024;
  config.api.unix_socket.path.clear();
  config.logging.file.clear();
  config.security.requirepass = "never-display-this";

  auto display = FormatConfigForDisplay(config);
  ASSERT_TRUE(display) << display.error().message();

  EXPECT_NE(display->find("max_memory_mb: 32"), std::string::npos);
  EXPECT_NE(display->find("unix_socket:"), std::string::npos);
  EXPECT_NE(display->find("path: \"\""), std::string::npos);
  EXPECT_NE(display->find("allow_cidrs:"), std::string::npos);
  EXPECT_NE(display->find("requirepass: \"***\""), std::string::npos);
  EXPECT_EQ(display->find("never-display-this"), std::string::npos);
}

/**
 * @brief Test loading configuration with invalid YAML syntax
 */
TEST(ConfigTest, LoadInvalidYAML) {
  // Create invalid YAML file
  const char* invalid_yaml = R"(
events:
  ctx_buffer_size: [unclosed array
  decay_alpha: 0.99
)";

  std::ofstream ofs("invalid_test_config.yaml");
  ofs << invalid_yaml;
  ofs.close();

  auto config_result = LoadConfig("invalid_test_config.yaml");
  EXPECT_FALSE(config_result);
  EXPECT_EQ(config_result.error().code(), nvecd::utils::ErrorCode::kConfigYamlError);

  // Cleanup
  std::remove("invalid_test_config.yaml");
}

/**
 * @brief Test configuration defaults
 */
TEST(ConfigTest, DefaultValues) {
  Config config;

  // Events defaults
  EXPECT_EQ(config.events.ctx_buffer_size, defaults::kCtxBufferSize);
  EXPECT_EQ(config.events.decay_interval_sec, defaults::kDecayIntervalSec);
  EXPECT_DOUBLE_EQ(config.events.decay_alpha, defaults::kDecayAlpha);

  // Vectors defaults
  EXPECT_EQ(config.vectors.default_dimension, defaults::kDefaultDimension);
  EXPECT_EQ(config.vectors.distance_metric, defaults::kDefaultDistanceMetric);

  // Similarity defaults
  EXPECT_EQ(config.similarity.default_top_k, defaults::kDefaultTopK);
  EXPECT_EQ(config.similarity.max_top_k, defaults::kMaxTopK);
  EXPECT_DOUBLE_EQ(config.similarity.fusion_alpha, defaults::kFusionAlpha);
  EXPECT_DOUBLE_EQ(config.similarity.fusion_beta, defaults::kFusionBeta);

  // API defaults
  EXPECT_EQ(config.api.tcp.port, defaults::kTcpPort);
  EXPECT_EQ(config.api.http.port, defaults::kHttpPort);
  EXPECT_FALSE(config.api.http.enable);

  // Performance defaults
  EXPECT_EQ(config.perf.thread_pool_size, defaults::kThreadPoolSize);
  EXPECT_EQ(config.perf.max_connections, defaults::kMaxConnections);
  EXPECT_EQ(config.perf.connection_timeout_sec, defaults::kConnectionTimeoutSec);
}

/**
 * @brief Test snapshot mode validation
 */
TEST(ConfigTest, ValidateSnapshotMode) {
  Config config;  // Defaults are valid

  // Valid modes
  config.snapshot.mode = "fork";
  EXPECT_TRUE(ValidateConfig(config));

  config.snapshot.mode = "lock";
  EXPECT_TRUE(ValidateConfig(config));

  // Invalid mode
  config.snapshot.mode = "invalid";
  auto result = ValidateConfig(config);
  EXPECT_FALSE(result);
  EXPECT_EQ(result.error().code(), nvecd::utils::ErrorCode::kConfigInvalidValue);
}

/**
 * @brief Test snapshot mode default value
 */
TEST(ConfigTest, SnapshotModeDefault) {
  Config config;
  EXPECT_EQ(config.snapshot.mode, "fork");
}
