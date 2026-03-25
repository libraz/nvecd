/**
 * @file variable_handler.h
 * @brief Handler for SET/GET/SHOW VARIABLES commands
 *
 * Reference: ../mygram-db/src/server/handlers/variable_handler.h
 * Reusability: 80% (namespace changes, nvecd-specific variables)
 */

#pragma once

#include <string>

#include "config/runtime_variable_manager.h"
#include "utils/error.h"
#include "utils/expected.h"

namespace nvecd::server::handlers {

/**
 * @brief Handle SET command
 * @param manager RuntimeVariableManager instance
 * @param variable_name Variable name (e.g., "logging.level")
 * @param value New value
 * @return Response string or error
 */
utils::Expected<std::string, utils::Error> HandleSet(config::RuntimeVariableManager* manager,
                                                     const std::string& variable_name, const std::string& value);

/**
 * @brief Handle SHOW VARIABLES command
 * @param manager RuntimeVariableManager instance
 * @param pattern Optional LIKE pattern filter
 * @return Response string or error
 */
utils::Expected<std::string, utils::Error> HandleShowVariables(config::RuntimeVariableManager* manager,
                                                               const std::string& pattern = "");

/**
 * @brief Handle GET command (single variable)
 * @param manager RuntimeVariableManager instance
 * @param variable_name Variable name
 * @return Response string or error
 */
utils::Expected<std::string, utils::Error> HandleGet(config::RuntimeVariableManager* manager,
                                                     const std::string& variable_name);

}  // namespace nvecd::server::handlers
