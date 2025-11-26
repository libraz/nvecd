/**
 * @file runtime_variable_manager.h
 * @brief Runtime variable manager for SET/SHOW VARIABLES support
 *
 * Reference: ../mygram-db/src/config/runtime_variable_manager.h
 * Reusability: 70% (removed MySQL-specific, focused on nvecd settings)
 */

#pragma once

#include <functional>
#include <map>
#include <memory>
#include <shared_mutex>
#include <string>
#include <utility>

#include "config/config.h"
#include "utils/error.h"
#include "utils/expected.h"

namespace nvecd::cache {
class SimilarityCache;
}  // namespace nvecd::cache

namespace nvecd::config {

/**
 * @brief Runtime variable information
 */
struct VariableInfo {
  std::string value;     ///< Current value as string
  bool mutable_{false};  ///< True if variable can be changed at runtime
};

/**
 * @brief Runtime variable manager (SET/SHOW VARIABLES)
 *
 * Responsibilities:
 * - Store runtime-modifiable configuration variables
 * - Validate variable changes before applying
 * - Apply changes to active components (logging, cache)
 * - Provide SHOW VARIABLES functionality
 *
 * Thread Safety: Thread-safe (uses shared_mutex)
 *
 * Mutable Variables (can be changed at runtime):
 * - logging.level (trace/debug/info/warn/error)
 * - logging.json (true/false for JSON vs text format)
 * - cache.enabled (true/false)
 * - cache.min_query_cost_ms (>=0)
 * - cache.ttl_seconds (>=0)
 *
 * Immutable Variables (require restart):
 * - api.tcp.*, api.http.* (socket binding)
 * - events.*, vectors.* (data structures)
 * - similarity.* (engine configuration)
 * - cache.max_memory_bytes (memory allocation)
 * - snapshot.* (snapshot configuration)
 * - perf.* (performance tuning)
 */
class RuntimeVariableManager {
 public:
  /**
   * @brief Create manager from initial config
   * @param initial_config Initial configuration
   * @return Expected with unique_ptr or error
   */
  static utils::Expected<std::unique_ptr<RuntimeVariableManager>, utils::Error> Create(const Config& initial_config);

  RuntimeVariableManager(const RuntimeVariableManager&) = delete;
  RuntimeVariableManager& operator=(const RuntimeVariableManager&) = delete;
  RuntimeVariableManager(RuntimeVariableManager&&) = delete;
  RuntimeVariableManager& operator=(RuntimeVariableManager&&) = delete;
  ~RuntimeVariableManager() = default;

  /**
   * @brief Set runtime variable (SET command)
   * @param variable_name Dot-separated name (e.g., "logging.level")
   * @param value New value as string
   * @return Expected with void or error
   *
   * Examples:
   * - SetVariable("logging.level", "debug")
   * - SetVariable("cache.enabled", "true")
   * - SetVariable("cache.ttl_seconds", "7200")
   */
  utils::Expected<void, utils::Error> SetVariable(const std::string& variable_name, const std::string& value);

  /**
   * @brief Get variable value
   * @param variable_name Dot-separated name
   * @return Expected with value string or error
   */
  utils::Expected<std::string, utils::Error> GetVariable(const std::string& variable_name) const;

  /**
   * @brief Get all variables with mutability info (SHOW VARIABLES)
   * @param prefix Optional prefix filter (e.g., "logging", "cache")
   * @return Map of variable_name -> VariableInfo
   */
  std::map<std::string, VariableInfo> GetAllVariables(const std::string& prefix = "") const;

  /**
   * @brief Check if variable is mutable
   * @param variable_name Dot-separated name
   * @return True if variable can be changed at runtime
   */
  static bool IsMutable(const std::string& variable_name);

  /**
   * @brief Set cache toggle callback
   * @param callback Function to call when cache.enabled changes
   */
  void SetCacheToggleCallback(std::function<utils::Expected<void, utils::Error>(bool enabled)> callback);

  /**
   * @brief Set similarity cache for runtime configuration updates
   * @param cache Pointer to SimilarityCache (non-owning)
   */
  void SetSimilarityCache(cache::SimilarityCache* cache);

 private:
  RuntimeVariableManager() = default;

  // Thread-safe storage (readers-writer lock)
  mutable std::shared_mutex mutex_;

  // Current runtime values (only mutable variables)
  std::map<std::string, std::string> runtime_values_;

  // Original config (immutable variables + defaults)
  Config base_config_;

  // Callbacks and component references
  std::function<utils::Expected<void, utils::Error>(bool enabled)> cache_toggle_callback_;
  cache::SimilarityCache* similarity_cache_ = nullptr;  // Non-owning pointer

  /**
   * @brief Apply logging.level change
   */
  static utils::Expected<void, utils::Error> ApplyLoggingLevel(const std::string& value);

  /**
   * @brief Apply logging.format change
   */
  static utils::Expected<void, utils::Error> ApplyLoggingFormat(const std::string& value);

  /**
   * @brief Apply cache.enabled change
   */
  utils::Expected<void, utils::Error> ApplyCacheEnabled(bool value);

  /**
   * @brief Apply cache.min_query_cost_ms change
   */
  utils::Expected<void, utils::Error> ApplyCacheMinQueryCost(double value);

  /**
   * @brief Apply cache.ttl_seconds change
   */
  utils::Expected<void, utils::Error> ApplyCacheTtl(int value);

  /**
   * @brief Get current value for a variable (internal, no lock)
   */
  std::string GetVariableInternal(const std::string& variable_name) const;

  /**
   * @brief Initialize runtime values from config
   */
  void InitializeRuntimeValues();

  /**
   * @brief Parse boolean value
   */
  static utils::Expected<bool, utils::Error> ParseBool(const std::string& value);

  /**
   * @brief Parse integer value
   */
  static utils::Expected<int, utils::Error> ParseInt(const std::string& value);

  /**
   * @brief Parse double value
   */
  static utils::Expected<double, utils::Error> ParseDouble(const std::string& value);
};

}  // namespace nvecd::config
