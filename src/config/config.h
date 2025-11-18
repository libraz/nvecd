/**
 * @file config.h
 * @brief Configuration structures and YAML parser for nvecd
 *
 * Reference: ../mygram-db/src/config/config.h
 * Reusability: 75% (removed MySQL/Table configs, added nvecd-specific configs)
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "utils/error.h"
#include "utils/expected.h"

namespace nvecd::config {

// Default values for configuration
namespace defaults {

// Event store defaults
constexpr uint32_t kCtxBufferSize = 50;
constexpr uint32_t kDecayIntervalSec = 3600;
constexpr double kDecayAlpha = 0.99;
constexpr uint32_t kDedupWindowSec = 60;     // 60 seconds deduplication window
constexpr uint32_t kDedupCacheSize = 10000;  // 10k entries in dedup cache

// Vector store defaults
constexpr uint32_t kDefaultDimension = 768;
constexpr const char* kDefaultDistanceMetric = "cosine";

// Similarity search defaults
constexpr uint32_t kDefaultTopK = 100;
constexpr uint32_t kMaxTopK = 1000;
constexpr double kFusionAlpha = 0.6;
constexpr double kFusionBeta = 0.4;

// Snapshot defaults
constexpr int kSnapshotIntervalSec = 0;  // 0 = disabled
constexpr const char* kSnapshotDefaultFilename = "nvecd.snapshot";

// API defaults
constexpr int kTcpPort = 11017;
constexpr int kHttpPort = 8080;

// Performance defaults
constexpr int kThreadPoolSize = 8;
constexpr int kMaxConnections = 1000;
constexpr int kConnectionTimeoutSec = 300;

}  // namespace defaults

/**
 * @brief Event store configuration
 */
struct EventsConfig {
  uint32_t ctx_buffer_size = defaults::kCtxBufferSize;        ///< Events per context (ring buffer size)
  uint32_t decay_interval_sec = defaults::kDecayIntervalSec;  ///< Decay interval in seconds
  double decay_alpha = defaults::kDecayAlpha;                 ///< Decay factor (0.0-1.0)
  uint32_t dedup_window_sec = defaults::kDedupWindowSec;      ///< Deduplication time window in seconds
  uint32_t dedup_cache_size = defaults::kDedupCacheSize;      ///< Deduplication cache size (LRU)
};

/**
 * @brief Vector store configuration
 */
struct VectorsConfig {
  uint32_t default_dimension = defaults::kDefaultDimension;        ///< Default vector dimension
  std::string distance_metric = defaults::kDefaultDistanceMetric;  ///< Distance metric: "cosine", "dot", "l2"
};

/**
 * @brief Similarity search configuration
 */
struct SimilarityConfig {
  uint32_t default_top_k = defaults::kDefaultTopK;  ///< Default number of results
  uint32_t max_top_k = defaults::kMaxTopK;          ///< Maximum number of results
  double fusion_alpha = defaults::kFusionAlpha;     ///< Weight for vector similarity in fusion mode
  double fusion_beta = defaults::kFusionBeta;       ///< Weight for co-occurrence in fusion mode
};

/**
 * @brief Snapshot configuration
 */
struct SnapshotConfig {
  std::string dir = "/var/lib/nvecd/snapshots";                       ///< Snapshot directory
  std::string default_filename = defaults::kSnapshotDefaultFilename;  ///< Default snapshot filename
  int interval_sec = defaults::kSnapshotIntervalSec;                  ///< Auto-snapshot interval (0 = disabled)
  int retain = 3;                                                     ///< Number of snapshots to retain
};

/**
 * @brief Performance configuration
 */
struct PerformanceConfig {
  int thread_pool_size = defaults::kThreadPoolSize;              ///< Worker thread pool size
  int max_connections = defaults::kMaxConnections;               ///< Maximum concurrent connections
  int connection_timeout_sec = defaults::kConnectionTimeoutSec;  ///< Connection timeout
};

/**
 * @brief API configuration
 */
struct ApiConfig {
  // Rate limiting defaults
  static constexpr int kDefaultRateLimitCapacity = 100;      ///< Default burst size
  static constexpr int kDefaultRateLimitRefillRate = 10;     ///< Default tokens per second
  static constexpr int kDefaultRateLimitMaxClients = 10000;  ///< Default max tracked clients

  struct {
    std::string bind = "127.0.0.1";
    int port = defaults::kTcpPort;
  } tcp;

  struct {
    bool enable = false;  // Disabled by default
    std::string bind = "127.0.0.1";
    int port = defaults::kHttpPort;
    bool enable_cors = false;
    std::string cors_allow_origin;
  } http;

  /**
   * @brief Rate limiting configuration (token bucket algorithm)
   */
  struct {
    bool enable = false;                            ///< Enable rate limiting (default: false)
    int capacity = kDefaultRateLimitCapacity;       ///< Maximum tokens per client (burst size)
    int refill_rate = kDefaultRateLimitRefillRate;  ///< Tokens added per second per client
    int max_clients = kDefaultRateLimitMaxClients;  ///< Maximum number of tracked clients
  } rate_limiting;
};

/**
 * @brief Network security configuration
 */
struct NetworkConfig {
  std::vector<std::string> allow_cidrs;  ///< Allowed CIDR ranges (empty = allow all)
};

/**
 * @brief Logging configuration
 */
struct LoggingConfig {
  std::string level = "info";  ///< Log level: trace, debug, info, warn, error
  bool json = true;            ///< Use structured JSON logging
  std::string file;            ///< Log file path (empty = stdout)
};

/**
 * @brief Cache configuration
 */
struct CacheConfig {
  bool enabled = true;                         ///< Enable/disable cache (default: true)
  size_t max_memory_bytes = 32 * 1024 * 1024;  ///< Maximum cache memory in bytes (default: 32MB)  // NOLINT
  double min_query_cost_ms = 10.0;             ///< Minimum query cost to cache (default: 10ms)  // NOLINT
  int ttl_seconds = 3600;                      ///< Cache entry TTL (default: 1 hour, 0 = no TTL)  // NOLINT

  // Advanced tuning
  bool compression_enabled = true;  ///< Enable LZ4 compression (default: true)
  int eviction_batch_size = 10;     ///< Number of entries to evict at once (default: 10)  // NOLINT
};

/**
 * @brief Root configuration
 */
struct Config {
  EventsConfig events;          ///< Event store configuration
  VectorsConfig vectors;        ///< Vector store configuration
  SimilarityConfig similarity;  ///< Similarity search configuration
  SnapshotConfig snapshot;      ///< Snapshot configuration
  PerformanceConfig perf;       ///< Performance configuration
  ApiConfig api;                ///< API configuration
  NetworkConfig network;        ///< Network security configuration
  LoggingConfig logging;        ///< Logging configuration
  CacheConfig cache;            ///< Cache configuration
};

/**
 * @brief Load configuration from YAML file
 *
 * @param path Path to YAML configuration file
 * @return Expected<Config, Error> with configuration or error
 */
utils::Expected<Config, utils::Error> LoadConfig(const std::string& path);

/**
 * @brief Validate configuration
 *
 * @param config Configuration to validate
 * @return Expected<void, Error> with success or validation error
 */
utils::Expected<void, utils::Error> ValidateConfig(const Config& config);

}  // namespace nvecd::config
