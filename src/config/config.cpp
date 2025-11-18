/**
 * @file config.cpp
 * @brief Configuration parser implementation for nvecd
 *
 * Reference: ../mygram-db/src/config/config.cpp
 * Reusability: 70% (YAML parsing pattern, adapted for nvecd-specific configs)
 */

#include "config/config.h"

#include <yaml-cpp/yaml.h>

#include <fstream>
#include <sstream>
#include <stdexcept>

#include <nlohmann/json-schema.hpp>
#include <nlohmann/json.hpp>

#include "config/config_schema_embedded.h"
#include "utils/error.h"
#include "utils/structured_log.h"

using nlohmann::json;
using nlohmann::json_schema::json_validator;

namespace nvecd::config {

namespace {

/**
 * @brief Convert YAML node to JSON (recursive)
 * Reference: ../mygram-db/src/config/config.cpp:YamlToJson
 *
 * @param yaml_node YAML node to convert
 * @return nlohmann::json JSON representation
 */
nlohmann::json YamlToJson(const YAML::Node& yaml_node) {
  if (yaml_node.IsNull()) {
    return nlohmann::json();
  }

  if (yaml_node.IsScalar()) {
    // Try different types
    try {
      return yaml_node.as<int64_t>();
    } catch (...) {
      try {
        return yaml_node.as<double>();
      } catch (...) {
        try {
          return yaml_node.as<bool>();
        } catch (...) {
          return yaml_node.as<std::string>();
        }
      }
    }
  }

  if (yaml_node.IsSequence()) {
    nlohmann::json json_array = nlohmann::json::array();
    for (const auto& item : yaml_node) {
      json_array.push_back(YamlToJson(item));
    }
    return json_array;
  }

  if (yaml_node.IsMap()) {
    nlohmann::json json_object;
    for (const auto& pair : yaml_node) {
      std::string key = pair.first.as<std::string>();
      json_object[key] = YamlToJson(pair.second);
    }
    return json_object;
  }

  return nlohmann::json();
}

/**
 * @brief Convert YAML node to JSON-like value for parsing
 * Reference: ../mygram-db/src/config/config.cpp:YamlToJson
 */
template <typename T>
T GetYamlValue(const YAML::Node& node, const std::string& key, const T& default_value) {
  if (!node[key]) {
    return default_value;
  }
  try {
    return node[key].as<T>();
  } catch (const YAML::Exception& e) {
    std::stringstream err;
    err << "Failed to parse config key '" << key << "': " << e.what();
    throw std::runtime_error(err.str());
  }
}

/**
 * @brief Parse events configuration
 */
EventsConfig ParseEventsConfig(const YAML::Node& node) {
  EventsConfig config;

  if (node["ctx_buffer_size"]) {
    config.ctx_buffer_size = node["ctx_buffer_size"].as<uint32_t>();
  }
  if (node["decay_interval_sec"]) {
    config.decay_interval_sec = node["decay_interval_sec"].as<uint32_t>();
  }
  if (node["decay_alpha"]) {
    config.decay_alpha = node["decay_alpha"].as<double>();
  }
  if (node["dedup_window_sec"]) {
    config.dedup_window_sec = node["dedup_window_sec"].as<uint32_t>();
  }
  if (node["dedup_cache_size"]) {
    config.dedup_cache_size = node["dedup_cache_size"].as<uint32_t>();
  }

  return config;
}

/**
 * @brief Parse vectors configuration
 */
VectorsConfig ParseVectorsConfig(const YAML::Node& node) {
  VectorsConfig config;

  if (node["default_dimension"]) {
    config.default_dimension = node["default_dimension"].as<uint32_t>();
  }
  if (node["distance_metric"]) {
    config.distance_metric = node["distance_metric"].as<std::string>();
  }

  return config;
}

/**
 * @brief Parse similarity configuration
 */
SimilarityConfig ParseSimilarityConfig(const YAML::Node& node) {
  SimilarityConfig config;

  if (node["default_top_k"]) {
    config.default_top_k = node["default_top_k"].as<uint32_t>();
  }
  if (node["max_top_k"]) {
    config.max_top_k = node["max_top_k"].as<uint32_t>();
  }
  if (node["fusion_alpha"]) {
    config.fusion_alpha = node["fusion_alpha"].as<double>();
  }
  if (node["fusion_beta"]) {
    config.fusion_beta = node["fusion_beta"].as<double>();
  }

  return config;
}

/**
 * @brief Parse snapshot configuration
 */
SnapshotConfig ParseSnapshotConfig(const YAML::Node& node) {
  SnapshotConfig config;

  if (node["dir"]) {
    config.dir = node["dir"].as<std::string>();
  }
  if (node["default_filename"]) {
    config.default_filename = node["default_filename"].as<std::string>();
  }
  if (node["interval_sec"]) {
    config.interval_sec = node["interval_sec"].as<int>();
  }
  if (node["retain"]) {
    config.retain = node["retain"].as<int>();
  }

  return config;
}

/**
 * @brief Parse performance configuration
 */
PerformanceConfig ParsePerformanceConfig(const YAML::Node& node) {
  PerformanceConfig config;

  if (node["thread_pool_size"]) {
    config.thread_pool_size = node["thread_pool_size"].as<int>();
  }
  if (node["max_connections"]) {
    config.max_connections = node["max_connections"].as<int>();
  }
  if (node["connection_timeout_sec"]) {
    config.connection_timeout_sec = node["connection_timeout_sec"].as<int>();
  }

  return config;
}

/**
 * @brief Parse API configuration
 */
ApiConfig ParseApiConfig(const YAML::Node& node) {
  ApiConfig config;

  // TCP configuration
  if (node["tcp"]) {
    const auto& tcp_node = node["tcp"];
    if (tcp_node["bind"]) {
      config.tcp.bind = tcp_node["bind"].as<std::string>();
    }
    if (tcp_node["port"]) {
      config.tcp.port = tcp_node["port"].as<int>();
    }
  }

  // HTTP configuration
  if (node["http"]) {
    const auto& http_node = node["http"];
    if (http_node["enable"]) {
      config.http.enable = http_node["enable"].as<bool>();
    }
    if (http_node["bind"]) {
      config.http.bind = http_node["bind"].as<std::string>();
    }
    if (http_node["port"]) {
      config.http.port = http_node["port"].as<int>();
    }
    if (http_node["enable_cors"]) {
      config.http.enable_cors = http_node["enable_cors"].as<bool>();
    }
    if (http_node["cors_allow_origin"]) {
      config.http.cors_allow_origin = http_node["cors_allow_origin"].as<std::string>();
    }
  }

  // Rate limiting configuration
  if (node["rate_limiting"]) {
    const auto& rl_node = node["rate_limiting"];
    if (rl_node["enable"]) {
      config.rate_limiting.enable = rl_node["enable"].as<bool>();
    }
    if (rl_node["capacity"]) {
      config.rate_limiting.capacity = rl_node["capacity"].as<int>();
    }
    if (rl_node["refill_rate"]) {
      config.rate_limiting.refill_rate = rl_node["refill_rate"].as<int>();
    }
    if (rl_node["max_clients"]) {
      config.rate_limiting.max_clients = rl_node["max_clients"].as<int>();
    }
  }

  return config;
}

/**
 * @brief Parse network configuration
 */
NetworkConfig ParseNetworkConfig(const YAML::Node& node) {
  NetworkConfig config;

  if (node["allow_cidrs"] && node["allow_cidrs"].IsSequence()) {
    for (const auto& cidr_node : node["allow_cidrs"]) {
      config.allow_cidrs.push_back(cidr_node.as<std::string>());
    }
  }

  return config;
}

/**
 * @brief Parse logging configuration
 */
LoggingConfig ParseLoggingConfig(const YAML::Node& node) {
  LoggingConfig config;

  if (node["level"]) {
    config.level = node["level"].as<std::string>();
  }
  if (node["json"]) {
    config.json = node["json"].as<bool>();
  }
  if (node["file"]) {
    config.file = node["file"].as<std::string>();
  }

  return config;
}

/**
 * @brief Parse cache configuration
 */
CacheConfig ParseCacheConfig(const YAML::Node& node) {
  CacheConfig config;

  if (node["enabled"]) {
    config.enabled = node["enabled"].as<bool>();
  }
  if (node["max_memory_mb"]) {
    // Convert MB to bytes
    config.max_memory_bytes = static_cast<size_t>(node["max_memory_mb"].as<int>()) * 1024 * 1024;
  }
  if (node["min_query_cost_ms"]) {
    config.min_query_cost_ms = node["min_query_cost_ms"].as<double>();
  }
  if (node["ttl_seconds"]) {
    config.ttl_seconds = node["ttl_seconds"].as<int>();
  }
  if (node["compression_enabled"]) {
    config.compression_enabled = node["compression_enabled"].as<bool>();
  }
  if (node["eviction_batch_size"]) {
    config.eviction_batch_size = node["eviction_batch_size"].as<int>();
  }

  return config;
}

/**
 * @brief Validate configuration against JSON Schema
 * Reference: ../mygram-db/src/config/config.cpp:ValidateConfigJson
 *
 * @param config_json JSON representation of configuration
 * @return Expected<void, Error> with success or validation error
 */
utils::Expected<void, utils::Error> ValidateConfigSchema(const nlohmann::json& config_json) {
  try {
    // Parse embedded schema
    json schema_json = json::parse(kConfigSchemaJson);

    // Create validator
    json_validator validator;
    validator.set_root_schema(schema_json);

    // Validate
    try {
      validator.validate(config_json);
      utils::StructuredLog().Event("config_validation").Field("status", "passed").Info();
    } catch (const std::exception& e) {
      std::stringstream err_msg;
      err_msg << "Configuration validation failed:\n";
      err_msg << "  " << e.what() << "\n\n";
      err_msg << "  Common configuration issues:\n";
      err_msg << "    - Missing required fields (vectors, events, etc.)\n";
      err_msg << "    - Invalid data types (string instead of number, etc.)\n";
      err_msg << "    - Invalid enum values (check allowed values)\n";
      err_msg << "    - Out of range values (check min/max constraints)\n\n";
      err_msg << "  Please check your configuration against the schema.\n";
      err_msg << "  Use 'CONFIG HELP <path>' to see configuration options.";
      return utils::MakeUnexpected(utils::MakeError(utils::ErrorCode::kConfigValidationError, err_msg.str()));
    }
  } catch (const json::parse_error& e) {
    return utils::MakeUnexpected(
        utils::MakeError(utils::ErrorCode::kConfigParseError, std::string("JSON parse error: ") + e.what()));
  }

  return {};
}

}  // namespace

utils::Expected<Config, utils::Error> LoadConfig(const std::string& path) {
  try {
    // Load YAML file
    YAML::Node root = YAML::LoadFile(path);

    // Convert to JSON for schema validation
    nlohmann::json config_json = YamlToJson(root);

    // Validate against JSON Schema
    auto validation_result = ValidateConfigSchema(config_json);
    if (!validation_result) {
      return utils::MakeUnexpected(validation_result.error());
    }

    Config config;

    // Parse each section
    if (root["events"]) {
      config.events = ParseEventsConfig(root["events"]);
    }
    if (root["vectors"]) {
      config.vectors = ParseVectorsConfig(root["vectors"]);
    }
    if (root["similarity"]) {
      config.similarity = ParseSimilarityConfig(root["similarity"]);
    }
    if (root["snapshot"]) {
      config.snapshot = ParseSnapshotConfig(root["snapshot"]);
    }
    if (root["performance"]) {
      config.perf = ParsePerformanceConfig(root["performance"]);
    }
    if (root["api"]) {
      config.api = ParseApiConfig(root["api"]);
    }
    if (root["network"]) {
      config.network = ParseNetworkConfig(root["network"]);
    }
    if (root["logging"]) {
      config.logging = ParseLoggingConfig(root["logging"]);
    }
    if (root["cache"]) {
      config.cache = ParseCacheConfig(root["cache"]);
    }

    // Validate configuration (semantic validation)
    auto semantic_validation = ValidateConfig(config);
    if (!semantic_validation) {
      return utils::MakeUnexpected(semantic_validation.error());
    }

    return config;

  } catch (const YAML::BadFile& e) {
    return utils::MakeUnexpected(
        utils::MakeError(utils::ErrorCode::kConfigFileNotFound, "Failed to open config file: " + std::string(e.what())));
  } catch (const YAML::Exception& e) {
    return utils::MakeUnexpected(
        utils::MakeError(utils::ErrorCode::kConfigYamlError, "YAML parsing error: " + std::string(e.what())));
  } catch (const std::exception& e) {
    return utils::MakeUnexpected(
        utils::MakeError(utils::ErrorCode::kConfigParseError, "Configuration error: " + std::string(e.what())));
  }
}

utils::Expected<void, utils::Error> ValidateConfig(const Config& config) {
  // Validate events configuration
  if (config.events.ctx_buffer_size == 0) {
    return utils::MakeUnexpected(
        utils::MakeError(utils::ErrorCode::kConfigInvalidValue, "events.ctx_buffer_size must be greater than 0"));
  }
  if (config.events.decay_alpha < 0.0 || config.events.decay_alpha > 1.0) {
    return utils::MakeUnexpected(
        utils::MakeError(utils::ErrorCode::kConfigInvalidValue, "events.decay_alpha must be between 0.0 and 1.0"));
  }

  // Validate vectors configuration
  if (config.vectors.default_dimension == 0) {
    return utils::MakeUnexpected(
        utils::MakeError(utils::ErrorCode::kConfigInvalidValue, "vectors.default_dimension must be greater than 0"));
  }
  if (config.vectors.distance_metric != "cosine" && config.vectors.distance_metric != "dot" &&
      config.vectors.distance_metric != "l2") {
    return utils::MakeUnexpected(utils::MakeError(
        utils::ErrorCode::kConfigInvalidValue,
        "vectors.distance_metric must be one of: cosine, dot, l2 (got: " + config.vectors.distance_metric + ")"));
  }

  // Validate similarity configuration
  if (config.similarity.default_top_k == 0) {
    return utils::MakeUnexpected(
        utils::MakeError(utils::ErrorCode::kConfigInvalidValue, "similarity.default_top_k must be greater than 0"));
  }
  if (config.similarity.max_top_k < config.similarity.default_top_k) {
    return utils::MakeUnexpected(
        utils::MakeError(utils::ErrorCode::kConfigInvalidValue, "similarity.max_top_k must be >= default_top_k"));
  }
  if (config.similarity.fusion_alpha < 0.0 || config.similarity.fusion_alpha > 1.0) {
    return utils::MakeUnexpected(
        utils::MakeError(utils::ErrorCode::kConfigInvalidValue, "similarity.fusion_alpha must be between 0.0 and 1.0"));
  }
  if (config.similarity.fusion_beta < 0.0 || config.similarity.fusion_beta > 1.0) {
    return utils::MakeUnexpected(
        utils::MakeError(utils::ErrorCode::kConfigInvalidValue, "similarity.fusion_beta must be between 0.0 and 1.0"));
  }

  // Validate snapshot configuration
  if (config.snapshot.interval_sec < 0) {
    return utils::MakeUnexpected(
        utils::MakeError(utils::ErrorCode::kConfigInvalidValue, "snapshot.interval_sec must be >= 0 (0 = disabled)"));
  }
  if (config.snapshot.retain < 0) {
    return utils::MakeUnexpected(
        utils::MakeError(utils::ErrorCode::kConfigInvalidValue, "snapshot.retain must be >= 0"));
  }

  // Validate performance configuration
  if (config.perf.thread_pool_size <= 0) {
    return utils::MakeUnexpected(
        utils::MakeError(utils::ErrorCode::kConfigInvalidValue, "performance.thread_pool_size must be greater than 0"));
  }
  if (config.perf.max_connections <= 0) {
    return utils::MakeUnexpected(
        utils::MakeError(utils::ErrorCode::kConfigInvalidValue, "performance.max_connections must be greater than 0"));
  }
  if (config.perf.connection_timeout_sec <= 0) {
    return utils::MakeUnexpected(
        utils::MakeError(utils::ErrorCode::kConfigInvalidValue, "performance.connection_timeout_sec must be greater than 0"));
  }

  // Validate API configuration
  if (config.api.tcp.port <= 0 || config.api.tcp.port > 65535) {
    return utils::MakeUnexpected(
        utils::MakeError(utils::ErrorCode::kConfigInvalidValue, "api.tcp.port must be between 1 and 65535"));
  }
  if (config.api.http.enable && (config.api.http.port <= 0 || config.api.http.port > 65535)) {
    return utils::MakeUnexpected(
        utils::MakeError(utils::ErrorCode::kConfigInvalidValue, "api.http.port must be between 1 and 65535"));
  }
  if (config.api.rate_limiting.enable) {
    if (config.api.rate_limiting.capacity <= 0) {
      return utils::MakeUnexpected(
          utils::MakeError(utils::ErrorCode::kConfigInvalidValue, "api.rate_limiting.capacity must be greater than 0"));
    }
    if (config.api.rate_limiting.refill_rate <= 0) {
      return utils::MakeUnexpected(
          utils::MakeError(utils::ErrorCode::kConfigInvalidValue, "api.rate_limiting.refill_rate must be greater than 0"));
    }
    if (config.api.rate_limiting.max_clients <= 0) {
      return utils::MakeUnexpected(
          utils::MakeError(utils::ErrorCode::kConfigInvalidValue, "api.rate_limiting.max_clients must be greater than 0"));
    }
  }

  // Validate logging configuration
  if (config.logging.level != "trace" && config.logging.level != "debug" && config.logging.level != "info" &&
      config.logging.level != "warn" && config.logging.level != "error") {
    return utils::MakeUnexpected(utils::MakeError(
        utils::ErrorCode::kConfigInvalidValue,
        "logging.level must be one of: trace, debug, info, warn, error (got: " + config.logging.level + ")"));
  }

  // Validate cache configuration
  if (config.cache.max_memory_bytes == 0 && config.cache.enabled) {
    return utils::MakeUnexpected(
        utils::MakeError(utils::ErrorCode::kConfigInvalidValue, "cache.max_memory_mb must be greater than 0 when cache is enabled"));
  }
  if (config.cache.ttl_seconds < 0) {
    return utils::MakeUnexpected(
        utils::MakeError(utils::ErrorCode::kConfigInvalidValue, "cache.ttl_seconds must be >= 0 (0 = no TTL)"));
  }

  return {};
}

}  // namespace nvecd::config
