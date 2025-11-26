/**
 * @file variable_handler.cpp
 * @brief Handler for SET/SHOW VARIABLES commands implementation
 *
 * Reference: ../mygram-db/src/server/handlers/variable_handler.cpp
 * Reusability: 80% (namespace changes, nvecd-specific variables)
 */

#include "server/handlers/variable_handler.h"

#include <algorithm>
#include <sstream>

namespace nvecd::server {

std::string VariableHandler::HandleSet(config::RuntimeVariableManager* manager, const std::string& variable_name,
                                       const std::string& value) {
  if (manager == nullptr) {
    return "-ERR RuntimeVariableManager not initialized\r\n";
  }

  auto result = manager->SetVariable(variable_name, value);
  if (!result) {
    return "-ERR " + result.error().message() + "\r\n";
  }

  return "+OK\r\n";
}

std::string VariableHandler::HandleShowVariables(config::RuntimeVariableManager* manager, const std::string& pattern) {
  if (manager == nullptr) {
    return "-ERR RuntimeVariableManager not initialized\r\n";
  }

  // Get all variables with optional prefix filter
  std::string prefix;
  if (!pattern.empty()) {
    // Handle LIKE pattern - convert simple wildcards to prefix
    // "cache.%" -> prefix "cache."
    // "logging.%" -> prefix "logging."
    size_t wildcard_pos = pattern.find('%');
    if (wildcard_pos != std::string::npos) {
      prefix = pattern.substr(0, wildcard_pos);
    } else {
      prefix = pattern;
    }
  }

  auto variables = manager->GetAllVariables(prefix);

  // Format output
  std::ostringstream oss;

  // Count lines for array response
  size_t count = 0;
  for (const auto& [name, info] : variables) {
    // If pattern has wildcard at end, match prefix
    // If no wildcard, match exact or prefix
    if (pattern.empty() || name.find(prefix) == 0) {
      count++;
    }
  }

  oss << "*" << count << "\r\n";

  for (const auto& [name, info] : variables) {
    if (pattern.empty() || name.find(prefix) == 0) {
      // Format: name=value (mutable) or name=value (immutable)
      oss << "$" << (name.size() + 1 + info.value.size() + (info.mutable_ ? 10 : 12)) << "\r\n";
      oss << name << "=" << info.value << (info.mutable_ ? " (mutable)" : " (immutable)") << "\r\n";
    }
  }

  return oss.str();
}

std::string VariableHandler::HandleGet(config::RuntimeVariableManager* manager, const std::string& variable_name) {
  if (manager == nullptr) {
    return "-ERR RuntimeVariableManager not initialized\r\n";
  }

  auto result = manager->GetVariable(variable_name);
  if (!result) {
    return "-ERR " + result.error().message() + "\r\n";
  }

  // Return as bulk string
  const std::string& value = *result;
  std::ostringstream oss;
  oss << "$" << value.size() << "\r\n" << value << "\r\n";
  return oss.str();
}

}  // namespace nvecd::server
