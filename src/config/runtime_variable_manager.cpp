/**
 * @file runtime_variable_manager.cpp
 * @brief Runtime variable manager implementation
 *
 * Reference: ../mygram-db/src/config/runtime_variable_manager.cpp
 * Reusability: 70% (removed MySQL-specific, focused on nvecd settings)
 */

#include "config/runtime_variable_manager.h"

#include <spdlog/spdlog.h>

#include <charconv>
#include <shared_mutex>

#include "cache/similarity_cache.h"
#include "utils/structured_log.h"

namespace nvecd::config {

using utils::Error;
using utils::ErrorCode;
using utils::Expected;
using utils::MakeError;
using utils::MakeUnexpected;

// Mutable variables (can be changed at runtime)
static const std::map<std::string, bool> kVariableMutability = {
    // Logging
    {"logging.level", true},
    {"logging.json", true},   // nvecd uses json bool, not format string
    {"logging.file", false},  // Immutable (requires file handle reopening)

    // Cache
    {"cache.enabled", true},
    {"cache.min_query_cost_ms", true},
    {"cache.ttl_seconds", true},
    {"cache.max_memory_bytes", false},     // Immutable (memory allocation)
    {"cache.compression_enabled", false},  // Immutable
    {"cache.eviction_batch_size", false},  // Immutable

    // API TCP settings (all immutable - require socket rebind)
    {"api.tcp.bind", false},
    {"api.tcp.port", false},

    // API HTTP settings (all immutable)
    {"api.http.enable", false},
    {"api.http.bind", false},
    {"api.http.port", false},
    {"api.http.enable_cors", false},
    {"api.http.cors_allow_origin", false},

    // API rate limiting
    {"api.rate_limiting.enable", false},  // Could be mutable in future
    {"api.rate_limiting.capacity", false},
    {"api.rate_limiting.refill_rate", false},
    {"api.rate_limiting.max_clients", false},

    // Event store (all immutable - data structures)
    {"events.ctx_buffer_size", false},
    {"events.decay_alpha", false},
    {"events.decay_interval_sec", false},
    {"events.dedup_window_sec", false},
    {"events.dedup_cache_size", false},

    // Vector store (all immutable)
    {"vectors.default_dimension", false},
    {"vectors.distance_metric", false},

    // Similarity (all immutable)
    {"similarity.fusion_alpha", false},
    {"similarity.fusion_beta", false},
    {"similarity.default_top_k", false},
    {"similarity.max_top_k", false},

    // Snapshot (all immutable)
    {"snapshot.dir", false},
    {"snapshot.default_filename", false},
    {"snapshot.interval_sec", false},
    {"snapshot.retain", false},

    // Performance (all immutable)
    {"perf.thread_pool_size", false},
    {"perf.max_connections", false},
    {"perf.connection_timeout_sec", false},
};

Expected<std::unique_ptr<RuntimeVariableManager>, Error> RuntimeVariableManager::Create(const Config& initial_config) {
  auto manager = std::unique_ptr<RuntimeVariableManager>(new RuntimeVariableManager());
  manager->base_config_ = initial_config;
  manager->InitializeRuntimeValues();
  return manager;
}

void RuntimeVariableManager::InitializeRuntimeValues() {
  // Initialize only mutable variables
  runtime_values_["logging.level"] = base_config_.logging.level;
  runtime_values_["logging.json"] = base_config_.logging.json ? "true" : "false";
  runtime_values_["cache.enabled"] = base_config_.cache.enabled ? "true" : "false";
  runtime_values_["cache.min_query_cost_ms"] = std::to_string(base_config_.cache.min_query_cost_ms);
  runtime_values_["cache.ttl_seconds"] = std::to_string(base_config_.cache.ttl_seconds);
}

Expected<void, Error> RuntimeVariableManager::SetVariable(const std::string& variable_name, const std::string& value) {
  // Check if variable exists
  auto var_iter = kVariableMutability.find(variable_name);
  if (var_iter == kVariableMutability.end()) {
    return MakeUnexpected(MakeError(ErrorCode::kInvalidArgument, "Unknown variable: " + variable_name));
  }

  // Check if variable is mutable
  if (!var_iter->second) {
    return MakeUnexpected(
        MakeError(ErrorCode::kInvalidArgument, "Variable '" + variable_name + "' is immutable (requires restart)"));
  }

  // Apply variable-specific logic
  Expected<void, Error> result;

  if (variable_name == "logging.level") {
    result = ApplyLoggingLevel(value);
  } else if (variable_name == "logging.json") {
    auto json_enabled = ParseBool(value);
    if (!json_enabled) {
      return MakeUnexpected(json_enabled.error());
    }
    result = ApplyLoggingFormat(*json_enabled ? "json" : "text");
  } else if (variable_name == "cache.enabled") {
    auto enabled = ParseBool(value);
    if (!enabled) {
      return MakeUnexpected(enabled.error());
    }
    result = ApplyCacheEnabled(*enabled);
  } else if (variable_name == "cache.min_query_cost_ms") {
    auto cost = ParseDouble(value);
    if (!cost) {
      return MakeUnexpected(cost.error());
    }
    result = ApplyCacheMinQueryCost(*cost);
  } else if (variable_name == "cache.ttl_seconds") {
    auto ttl = ParseInt(value);
    if (!ttl) {
      return MakeUnexpected(ttl.error());
    }
    result = ApplyCacheTtl(*ttl);
  } else {
    return MakeUnexpected(MakeError(ErrorCode::kInvalidArgument, "Variable not implemented: " + variable_name));
  }

  if (!result) {
    return result;
  }

  // Update runtime value (with lock)
  {
    std::unique_lock lock(mutex_);
    runtime_values_[variable_name] = value;
  }

  // Log the change
  utils::StructuredLog().Event("variable_changed").Field("variable", variable_name).Field("value", value).Info();

  return {};
}

Expected<std::string, Error> RuntimeVariableManager::GetVariable(const std::string& variable_name) const {
  std::shared_lock lock(mutex_);
  std::string value = GetVariableInternal(variable_name);
  if (value.empty()) {
    return MakeUnexpected(MakeError(ErrorCode::kInvalidArgument, "Unknown variable: " + variable_name));
  }
  return value;
}

std::map<std::string, VariableInfo> RuntimeVariableManager::GetAllVariables(const std::string& prefix) const {
  std::shared_lock lock(mutex_);
  std::map<std::string, VariableInfo> result;

  // Add all known variables
  for (const auto& [name, is_mutable] : kVariableMutability) {
    if (!prefix.empty() && name.find(prefix) != 0) {
      continue;  // Skip if doesn't match prefix
    }

    std::string value = GetVariableInternal(name);
    if (!value.empty()) {
      result[name] = {value, is_mutable};
    }
  }

  return result;
}

bool RuntimeVariableManager::IsMutable(const std::string& variable_name) {
  auto var_iter = kVariableMutability.find(variable_name);
  return (var_iter != kVariableMutability.end()) && var_iter->second;
}

void RuntimeVariableManager::SetCacheToggleCallback(std::function<Expected<void, Error>(bool enabled)> callback) {
  cache_toggle_callback_ = std::move(callback);
}

void RuntimeVariableManager::SetSimilarityCache(cache::SimilarityCache* cache) {
  similarity_cache_ = cache;
}

// ========== Apply functions ==========

Expected<void, Error> RuntimeVariableManager::ApplyLoggingLevel(const std::string& value) {
  // Validate and apply logging level
  if (value != "trace" && value != "debug" && value != "info" && value != "warn" && value != "error") {
    return MakeUnexpected(MakeError(ErrorCode::kInvalidArgument,
                                    "Invalid logging level (must be trace/debug/info/warn/error): " + value));
  }

  // Apply to spdlog
  if (value == "trace") {
    spdlog::set_level(spdlog::level::trace);
  } else if (value == "debug") {
    spdlog::set_level(spdlog::level::debug);
  } else if (value == "info") {
    spdlog::set_level(spdlog::level::info);
  } else if (value == "warn") {
    spdlog::set_level(spdlog::level::warn);
  } else if (value == "error") {
    spdlog::set_level(spdlog::level::err);
  }

  return {};
}

Expected<void, Error> RuntimeVariableManager::ApplyLoggingFormat(const std::string& value) {
  // Validate format
  if (value != "json" && value != "text") {
    return MakeUnexpected(
        MakeError(ErrorCode::kInvalidArgument, "Invalid logging format (must be json/text): " + value));
  }

  // Apply to structured log
  utils::StructuredLog::SetFormat(utils::StructuredLog::ParseFormat(value));

  return {};
}

Expected<void, Error> RuntimeVariableManager::ApplyCacheEnabled(bool value) {
  if (cache_toggle_callback_) {
    return cache_toggle_callback_(value);
  }
  // No callback registered - just update the runtime value
  return {};
}

Expected<void, Error> RuntimeVariableManager::ApplyCacheMinQueryCost(double value) {
  if (value < 0) {
    return MakeUnexpected(MakeError(ErrorCode::kInvalidArgument, "cache.min_query_cost_ms must be >= 0"));
  }

  if (similarity_cache_) {
    similarity_cache_->SetMinQueryCost(value);
  }

  return {};
}

Expected<void, Error> RuntimeVariableManager::ApplyCacheTtl(int value) {
  if (value < 0) {
    return MakeUnexpected(MakeError(ErrorCode::kInvalidArgument, "cache.ttl_seconds must be >= 0"));
  }

  if (similarity_cache_) {
    similarity_cache_->SetTtl(value);
  }

  return {};
}

std::string RuntimeVariableManager::GetVariableInternal(const std::string& variable_name) const {
  // Check runtime values first (for mutable variables)
  auto runtime_iter = runtime_values_.find(variable_name);
  if (runtime_iter != runtime_values_.end()) {
    return runtime_iter->second;
  }

  // Fall back to base config for immutable variables

  // Logging
  if (variable_name == "logging.file") {
    return base_config_.logging.file;
  }

  // Cache
  if (variable_name == "cache.max_memory_bytes") {
    return std::to_string(base_config_.cache.max_memory_bytes);
  }
  if (variable_name == "cache.compression_enabled") {
    return base_config_.cache.compression_enabled ? "true" : "false";
  }
  if (variable_name == "cache.eviction_batch_size") {
    return std::to_string(base_config_.cache.eviction_batch_size);
  }

  // API TCP
  if (variable_name == "api.tcp.bind") {
    return base_config_.api.tcp.bind;
  }
  if (variable_name == "api.tcp.port") {
    return std::to_string(base_config_.api.tcp.port);
  }

  // API HTTP
  if (variable_name == "api.http.enable") {
    return base_config_.api.http.enable ? "true" : "false";
  }
  if (variable_name == "api.http.bind") {
    return base_config_.api.http.bind;
  }
  if (variable_name == "api.http.port") {
    return std::to_string(base_config_.api.http.port);
  }
  if (variable_name == "api.http.enable_cors") {
    return base_config_.api.http.enable_cors ? "true" : "false";
  }
  if (variable_name == "api.http.cors_allow_origin") {
    return base_config_.api.http.cors_allow_origin;
  }

  // API rate limiting
  if (variable_name == "api.rate_limiting.enable") {
    return base_config_.api.rate_limiting.enable ? "true" : "false";
  }
  if (variable_name == "api.rate_limiting.capacity") {
    return std::to_string(base_config_.api.rate_limiting.capacity);
  }
  if (variable_name == "api.rate_limiting.refill_rate") {
    return std::to_string(base_config_.api.rate_limiting.refill_rate);
  }
  if (variable_name == "api.rate_limiting.max_clients") {
    return std::to_string(base_config_.api.rate_limiting.max_clients);
  }

  // Events
  if (variable_name == "events.ctx_buffer_size") {
    return std::to_string(base_config_.events.ctx_buffer_size);
  }
  if (variable_name == "events.decay_alpha") {
    return std::to_string(base_config_.events.decay_alpha);
  }
  if (variable_name == "events.decay_interval_sec") {
    return std::to_string(base_config_.events.decay_interval_sec);
  }
  if (variable_name == "events.dedup_window_sec") {
    return std::to_string(base_config_.events.dedup_window_sec);
  }
  if (variable_name == "events.dedup_cache_size") {
    return std::to_string(base_config_.events.dedup_cache_size);
  }

  // Vectors
  if (variable_name == "vectors.default_dimension") {
    return std::to_string(base_config_.vectors.default_dimension);
  }
  if (variable_name == "vectors.distance_metric") {
    return base_config_.vectors.distance_metric;
  }

  // Similarity
  if (variable_name == "similarity.fusion_alpha") {
    return std::to_string(base_config_.similarity.fusion_alpha);
  }
  if (variable_name == "similarity.fusion_beta") {
    return std::to_string(base_config_.similarity.fusion_beta);
  }
  if (variable_name == "similarity.default_top_k") {
    return std::to_string(base_config_.similarity.default_top_k);
  }
  if (variable_name == "similarity.max_top_k") {
    return std::to_string(base_config_.similarity.max_top_k);
  }

  // Snapshot
  if (variable_name == "snapshot.dir") {
    return base_config_.snapshot.dir;
  }
  if (variable_name == "snapshot.default_filename") {
    return base_config_.snapshot.default_filename;
  }
  if (variable_name == "snapshot.interval_sec") {
    return std::to_string(base_config_.snapshot.interval_sec);
  }
  if (variable_name == "snapshot.retain") {
    return std::to_string(base_config_.snapshot.retain);
  }

  // Performance
  if (variable_name == "perf.thread_pool_size") {
    return std::to_string(base_config_.perf.thread_pool_size);
  }
  if (variable_name == "perf.max_connections") {
    return std::to_string(base_config_.perf.max_connections);
  }
  if (variable_name == "perf.connection_timeout_sec") {
    return std::to_string(base_config_.perf.connection_timeout_sec);
  }

  return "";  // Unknown variable
}

// ========== Parse helpers ==========

Expected<bool, Error> RuntimeVariableManager::ParseBool(const std::string& value) {
  if (value == "true" || value == "on" || value == "1" || value == "yes") {
    return true;
  }
  if (value == "false" || value == "off" || value == "0" || value == "no") {
    return false;
  }
  return MakeUnexpected(
      MakeError(ErrorCode::kInvalidArgument, "Invalid boolean value (use true/false, on/off, 1/0): " + value));
}

Expected<int, Error> RuntimeVariableManager::ParseInt(const std::string& value) {
  int result = 0;
  auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), result);
  if (ec != std::errc{} || ptr != value.data() + value.size()) {
    return MakeUnexpected(MakeError(ErrorCode::kInvalidArgument, "Invalid integer value: " + value));
  }
  return result;
}

Expected<double, Error> RuntimeVariableManager::ParseDouble(const std::string& value) {
  try {
    size_t pos = 0;
    double result = std::stod(value, &pos);
    if (pos != value.size()) {
      return MakeUnexpected(MakeError(ErrorCode::kInvalidArgument, "Invalid double value: " + value));
    }
    return result;
  } catch (const std::exception&) {
    return MakeUnexpected(MakeError(ErrorCode::kInvalidArgument, "Invalid double value: " + value));
  }
}

}  // namespace nvecd::config
