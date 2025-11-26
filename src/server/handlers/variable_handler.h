/**
 * @file variable_handler.h
 * @brief Handler for SET/SHOW VARIABLES commands
 *
 * Reference: ../mygram-db/src/server/handlers/variable_handler.h
 * Reusability: 80% (namespace changes, nvecd-specific variables)
 */

#pragma once

#include <string>

#include "config/runtime_variable_manager.h"

namespace nvecd::server {

/**
 * @brief Handler for runtime variable commands
 *
 * Handles SET and SHOW VARIABLES commands for runtime configuration.
 */
class VariableHandler {
 public:
  /**
   * @brief Handle SET command
   * @param manager RuntimeVariableManager instance
   * @param variable_name Variable name (e.g., "logging.level")
   * @param value New value
   * @return Response string (OK or error)
   *
   * Example: SET logging.level debug
   */
  static std::string HandleSet(config::RuntimeVariableManager* manager, const std::string& variable_name,
                               const std::string& value);

  /**
   * @brief Handle SHOW VARIABLES command
   * @param manager RuntimeVariableManager instance
   * @param pattern Optional LIKE pattern filter
   * @return Response string (variable list)
   *
   * Example: SHOW VARIABLES
   * Example: SHOW VARIABLES LIKE cache.%
   */
  static std::string HandleShowVariables(config::RuntimeVariableManager* manager, const std::string& pattern = "");

  /**
   * @brief Handle GET command (single variable)
   * @param manager RuntimeVariableManager instance
   * @param variable_name Variable name
   * @return Response string (value or error)
   *
   * Example: GET logging.level
   */
  static std::string HandleGet(config::RuntimeVariableManager* manager, const std::string& variable_name);
};

}  // namespace nvecd::server
