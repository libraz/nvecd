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
#include <cmath>
#include <shared_mutex>
#include <string_view>

#include "cache/similarity_cache_controller.h"
#include "utils/structured_log.h"

namespace nvecd::config {

using utils::Error;
using utils::ErrorCode;
using utils::Expected;
using utils::MakeError;
using utils::MakeUnexpected;

namespace {

// Configuration files and the JSON schema use `performance.*`.  Accept the
// historical `perf.*` spelling on the command surface, but expose only the
// schema spelling from SHOW VARIABLES and related introspection.
std::string CanonicalVariableName(const std::string& variable_name) {
  constexpr std::string_view kLegacyPrefix = "perf.";
  if (variable_name.compare(0, kLegacyPrefix.size(), kLegacyPrefix) == 0) {
    return "performance." + variable_name.substr(kLegacyPrefix.size());
  }
  return variable_name;
}

std::string CanonicalDouble(double value) {
  char buffer[64];
  const auto [end, ec] = std::to_chars(buffer, buffer + sizeof(buffer), value, std::chars_format::general);
  if (ec == std::errc{}) {
    return {buffer, end};
  }
  // ParseDouble already guarantees a finite value. This fallback is only for
  // platforms whose floating-point to_chars implementation reports an error.
  return std::to_string(value);
}

}  // namespace

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
    {"cache.max_memory_mb", false},        // Immutable (memory allocation)
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
    {"events.max_contexts", false},
    {"events.max_neighbors_per_item", false},
    {"events.min_support", false},
    {"events.decay_alpha", false},
    {"events.decay_interval_sec", false},
    {"events.dedup_window_sec", false},
    {"events.dedup_cache_size", false},
    {"events.temporal_cooccurrence", false},  // Feature flag (startup-only)
    {"events.negative_signals", false},       // Feature flag (startup-only)

    // Vector store (all immutable)
    {"vectors.default_dimension", false},
    {"vectors.distance_metric", false},

    // Similarity (all immutable)
    {"similarity.fusion_alpha", false},
    {"similarity.fusion_beta", false},
    {"similarity.default_top_k", false},
    {"similarity.max_top_k", false},
    {"similarity.adaptive_fusion", false},  // Feature flag (startup-only)
    {"similarity.index_type", false},       // Active ANN index (startup-only)
    {"similarity.ivf_enabled", false},      // Feature flag (startup-only)

    // Snapshot (all immutable)
    {"snapshot.dir", false},
    {"snapshot.default_filename", false},
    {"snapshot.interval_sec", false},
    {"snapshot.retain", false},
    {"snapshot.mode", false},

    // Performance (all immutable)
    {"performance.thread_pool_size", false},
    {"performance.max_connections", false},
    {"performance.max_connections_per_ip", false},
    {"performance.connection_timeout_sec", false},
    {"performance.recv_buffer_size", false},
    {"performance.send_buffer_size", false},
    {"performance.max_query_length", false},
    {"performance.shutdown_timeout_ms", false},
    {"performance.reactor_max_total_buffered_bytes", false},

    // API HTTP timeout (immutable)
    {"api.http.timeout_sec", false},
    {"api.unix_socket.path", false},
    {"events.temporal_half_life_sec", false},
    {"events.negative_weight", false},
    {"similarity.sample_size", false},
    {"similarity.adaptive_min_alpha", false},
    {"similarity.adaptive_max_alpha", false},
    {"similarity.adaptive_maturity_threshold", false},
    {"similarity.ivf_nlist", false},
    {"similarity.ivf_nprobe", false},
    {"similarity.ivf_train_threshold", false},
    {"similarity.ivf_seal_threshold", false},
    {"similarity.hnsw_m", false},
    {"similarity.hnsw_ef_construction", false},
    {"similarity.hnsw_ef_search", false},
    {"similarity.hnsw_max_elements", false},
    {"network.allow_cidrs", false},
    {"security.requirepass", false},
    {"wal.enabled", false},
    {"wal.dir", false},
    {"wal.max_file_size", false},
    {"wal.sync_on_write", false},
    {"wal.sync_interval_ms", false},
    {"wal.include_vectors", false},
};

RuntimeVariableManager::RuntimeVariableManager(ConstructorTag) {}

Expected<std::unique_ptr<RuntimeVariableManager>, Error> RuntimeVariableManager::Create(const Config& initial_config) {
  auto manager = std::make_unique<RuntimeVariableManager>(ConstructorTag{});
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
  const auto canonical_name = CanonicalVariableName(variable_name);
  // Check if variable exists
  auto var_iter = kVariableMutability.find(canonical_name);
  if (var_iter == kVariableMutability.end()) {
    return MakeUnexpected(MakeError(ErrorCode::kInvalidArgument, "Unknown variable: " + variable_name));
  }

  // Check if variable is mutable
  if (!var_iter->second) {
    return MakeUnexpected(
        MakeError(ErrorCode::kInvalidArgument, "Variable '" + variable_name + "' is immutable (requires restart)"));
  }

  // Parse before changing process-wide component state, then store a canonical
  // representation. CONFIG SHOW and a subsequent CONFIG SET therefore round
  // trip regardless of whether callers used aliases such as "on" or "1".
  Expected<void, Error> result;
  std::string canonical_value;
  // Serializing the apply + map-update sequence prevents a slower component
  // callback from making runtime_values_ disagree with the actual last-applied
  // setting when multiple CONFIG SET requests arrive concurrently.
  std::scoped_lock set_lock(set_mutex_);

  if (canonical_name == "logging.level") {
    canonical_value = value;
    result = ApplyLoggingLevel(value);
  } else if (canonical_name == "logging.json") {
    auto json_enabled = ParseBool(value);
    if (!json_enabled) {
      return MakeUnexpected(json_enabled.error());
    }
    canonical_value = *json_enabled ? "true" : "false";
    result = ApplyLoggingFormat(*json_enabled ? "json" : "text");
  } else if (canonical_name == "cache.enabled") {
    auto enabled = ParseBool(value);
    if (!enabled) {
      return MakeUnexpected(enabled.error());
    }
    canonical_value = *enabled ? "true" : "false";
    result = ApplyCacheEnabled(*enabled);
  } else if (canonical_name == "cache.min_query_cost_ms") {
    auto cost = ParseDouble(value);
    if (!cost) {
      return MakeUnexpected(cost.error());
    }
    canonical_value = CanonicalDouble(*cost);
    result = ApplyCacheMinQueryCost(*cost);
  } else if (canonical_name == "cache.ttl_seconds") {
    auto ttl = ParseInt(value);
    if (!ttl) {
      return MakeUnexpected(ttl.error());
    }
    canonical_value = std::to_string(*ttl);
    result = ApplyCacheTtl(*ttl);
  } else {
    return MakeUnexpected(MakeError(ErrorCode::kInvalidArgument, "Variable not implemented: " + variable_name));
  }

  if (!result) {
    return result;
  }

  {
    std::unique_lock lock(mutex_);
    runtime_values_[canonical_name] = canonical_value;
  }

  // Log the change
  utils::StructuredLog()
      .Event("variable_changed")
      .Field("variable", canonical_name)
      .Field("value", canonical_value)
      .Info();

  return {};
}

Expected<std::string, Error> RuntimeVariableManager::GetVariable(const std::string& variable_name) const {
  std::shared_lock lock(mutex_);
  const auto canonical_name = CanonicalVariableName(variable_name);
  if (kVariableMutability.find(canonical_name) == kVariableMutability.end()) {
    return MakeUnexpected(MakeError(ErrorCode::kInvalidArgument, "Unknown variable: " + variable_name));
  }
  return GetVariableInternal(canonical_name);
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
    result[name] = {value, is_mutable};
  }

  return result;
}

bool RuntimeVariableManager::IsMutable(const std::string& variable_name) {
  auto var_iter = kVariableMutability.find(CanonicalVariableName(variable_name));
  return (var_iter != kVariableMutability.end()) && var_iter->second;
}

void RuntimeVariableManager::SetCacheController(cache::SimilarityCacheController* controller) {
  cache_controller_ = controller;
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
  if (cache_controller_ == nullptr) {
    return MakeUnexpected(MakeError(ErrorCode::kInternalError, "Cache controller is not initialized"));
  }
  return cache_controller_->SetEnabled(value);
}

Expected<void, Error> RuntimeVariableManager::ApplyCacheMinQueryCost(double value) {
  if (value < 0) {
    return MakeUnexpected(MakeError(ErrorCode::kInvalidArgument, "cache.min_query_cost_ms must be >= 0"));
  }

  if (cache_controller_ == nullptr) {
    return MakeUnexpected(MakeError(ErrorCode::kInternalError, "Cache controller is not initialized"));
  }
  return cache_controller_->SetMinQueryCost(value);
}

Expected<void, Error> RuntimeVariableManager::ApplyCacheTtl(int value) {
  if (value < 0) {
    return MakeUnexpected(MakeError(ErrorCode::kInvalidArgument, "cache.ttl_seconds must be >= 0"));
  }

  if (cache_controller_ == nullptr) {
    return MakeUnexpected(MakeError(ErrorCode::kInternalError, "Cache controller is not initialized"));
  }
  return cache_controller_->SetTtl(value);
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
  if (variable_name == "cache.max_memory_mb") {
    constexpr size_t kBytesPerMB = 1024 * 1024;
    return std::to_string(base_config_.cache.max_memory_bytes / kBytesPerMB);
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
  if (variable_name == "events.max_contexts") {
    return std::to_string(base_config_.events.max_contexts);
  }
  if (variable_name == "events.max_neighbors_per_item") {
    return std::to_string(base_config_.events.max_neighbors_per_item);
  }
  if (variable_name == "events.min_support") {
    return std::to_string(base_config_.events.min_support);
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
  if (variable_name == "events.temporal_cooccurrence") {
    return base_config_.events.temporal_cooccurrence ? "true" : "false";
  }
  if (variable_name == "events.negative_signals") {
    return base_config_.events.negative_signals ? "true" : "false";
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
  if (variable_name == "similarity.adaptive_fusion") {
    return base_config_.similarity.adaptive_fusion ? "true" : "false";
  }
  if (variable_name == "similarity.index_type") {
    return base_config_.similarity.index_type;
  }
  if (variable_name == "similarity.ivf_enabled") {
    return base_config_.similarity.ivf_enabled ? "true" : "false";
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
  if (variable_name == "snapshot.mode") {
    return base_config_.snapshot.mode;
  }

  // Performance
  if (variable_name == "performance.thread_pool_size") {
    return std::to_string(base_config_.perf.thread_pool_size);
  }
  if (variable_name == "performance.max_connections") {
    return std::to_string(base_config_.perf.max_connections);
  }
  if (variable_name == "performance.max_connections_per_ip") {
    return std::to_string(base_config_.perf.max_connections_per_ip);
  }
  if (variable_name == "performance.connection_timeout_sec") {
    return std::to_string(base_config_.perf.connection_timeout_sec);
  }
  if (variable_name == "performance.recv_buffer_size") {
    return std::to_string(base_config_.perf.recv_buffer_size);
  }
  if (variable_name == "performance.send_buffer_size") {
    return std::to_string(base_config_.perf.send_buffer_size);
  }
  if (variable_name == "performance.max_query_length") {
    return std::to_string(base_config_.perf.max_query_length);
  }
  if (variable_name == "performance.shutdown_timeout_ms") {
    return std::to_string(base_config_.perf.shutdown_timeout_ms);
  }
  if (variable_name == "performance.reactor_max_total_buffered_bytes") {
    return std::to_string(base_config_.perf.reactor_max_total_buffered_bytes);
  }

  // API HTTP timeout
  if (variable_name == "api.http.timeout_sec") {
    return std::to_string(base_config_.api.http.timeout_sec);
  }
  if (variable_name == "api.unix_socket.path")
    return base_config_.api.unix_socket.path;
  if (variable_name == "events.temporal_half_life_sec")
    return std::to_string(base_config_.events.temporal_half_life_sec);
  if (variable_name == "events.negative_weight")
    return std::to_string(base_config_.events.negative_weight);
  if (variable_name == "similarity.sample_size")
    return std::to_string(base_config_.similarity.sample_size);
  if (variable_name == "similarity.adaptive_min_alpha")
    return std::to_string(base_config_.similarity.adaptive_min_alpha);
  if (variable_name == "similarity.adaptive_max_alpha")
    return std::to_string(base_config_.similarity.adaptive_max_alpha);
  if (variable_name == "similarity.adaptive_maturity_threshold")
    return std::to_string(base_config_.similarity.adaptive_maturity_threshold);
  if (variable_name == "similarity.ivf_nlist")
    return std::to_string(base_config_.similarity.ivf_nlist);
  if (variable_name == "similarity.ivf_nprobe")
    return std::to_string(base_config_.similarity.ivf_nprobe);
  if (variable_name == "similarity.ivf_train_threshold")
    return std::to_string(base_config_.similarity.ivf_train_threshold);
  if (variable_name == "similarity.ivf_seal_threshold")
    return std::to_string(base_config_.similarity.ivf_seal_threshold);
  if (variable_name == "similarity.hnsw_m")
    return std::to_string(base_config_.similarity.hnsw_m);
  if (variable_name == "similarity.hnsw_ef_construction")
    return std::to_string(base_config_.similarity.hnsw_ef_construction);
  if (variable_name == "similarity.hnsw_ef_search")
    return std::to_string(base_config_.similarity.hnsw_ef_search);
  if (variable_name == "similarity.hnsw_max_elements")
    return std::to_string(base_config_.similarity.hnsw_max_elements);
  if (variable_name == "network.allow_cidrs") {
    std::string value;
    for (size_t i = 0; i < base_config_.network.allow_cidrs.size(); ++i) {
      if (i != 0)
        value += ',';
      value += base_config_.network.allow_cidrs[i];
    }
    return value;
  }
  if (variable_name == "security.requirepass")
    return base_config_.security.requirepass.empty() ? "" : "***";
  if (variable_name == "wal.enabled")
    return base_config_.wal.enabled ? "true" : "false";
  if (variable_name == "wal.dir")
    return base_config_.wal.dir;
  if (variable_name == "wal.max_file_size")
    return std::to_string(base_config_.wal.max_file_size);
  if (variable_name == "wal.sync_on_write")
    return base_config_.wal.sync_on_write ? "true" : "false";
  if (variable_name == "wal.sync_interval_ms")
    return std::to_string(base_config_.wal.sync_interval_ms);
  if (variable_name == "wal.include_vectors")
    return base_config_.wal.include_vectors ? "true" : "false";

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
    if (pos != value.size() || !std::isfinite(result)) {
      return MakeUnexpected(MakeError(ErrorCode::kInvalidArgument, "Invalid double value: " + value));
    }
    return result;
  } catch (const std::exception&) {
    return MakeUnexpected(MakeError(ErrorCode::kInvalidArgument, "Invalid double value: " + value));
  }
}

}  // namespace nvecd::config
