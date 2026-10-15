/**
 * @file variable_handler.cpp
 * @brief Handler for SET/GET/SHOW VARIABLES commands implementation
 *
 * Reference: ../mygram-db/src/server/handlers/variable_handler.cpp
 * Reusability: 80% (namespace changes, nvecd-specific variables)
 */

#include "server/handlers/variable_handler.h"

#include <sstream>
#include <string_view>

namespace nvecd::server::handlers {

namespace {
constexpr std::string_view kMutableSuffix = " (mutable)";
constexpr std::string_view kImmutableSuffix = " (immutable)";
}  // namespace

utils::Expected<std::string, utils::Error> HandleSet(config::RuntimeVariableManager* manager,
                                                     const std::string& variable_name, const std::string& value) {
  if (manager == nullptr) {
    return utils::MakeUnexpected(
        utils::MakeError(utils::ErrorCode::kInternalError, "RuntimeVariableManager not initialized"));
  }

  auto result = manager->SetVariable(variable_name, value);
  if (!result) {
    return utils::MakeUnexpected(result.error());
  }

  return std::string("+OK\r\n");
}

utils::Expected<std::string, utils::Error> HandleShowVariables(config::RuntimeVariableManager* manager,
                                                               const std::string& pattern) {
  if (manager == nullptr) {
    return utils::MakeUnexpected(
        utils::MakeError(utils::ErrorCode::kInternalError, "RuntimeVariableManager not initialized"));
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

  oss << "*" << variables.size() << "\r\n";

  for (const auto& [name, info] : variables) {
    const std::string_view suffix = info.mutable_ ? kMutableSuffix : kImmutableSuffix;
    oss << "$" << (name.size() + 1 + info.value.size() + suffix.size()) << "\r\n";
    oss << name << "=" << info.value << suffix << "\r\n";
  }

  return oss.str();
}

utils::Expected<std::string, utils::Error> HandleGet(config::RuntimeVariableManager* manager,
                                                     const std::string& variable_name) {
  if (manager == nullptr) {
    return utils::MakeUnexpected(
        utils::MakeError(utils::ErrorCode::kInternalError, "RuntimeVariableManager not initialized"));
  }

  auto result = manager->GetVariable(variable_name);
  if (!result) {
    return utils::MakeUnexpected(result.error());
  }

  // Return as bulk string
  const std::string& val = *result;
  std::ostringstream oss;
  oss << "$" << val.size() << "\r\n" << val << "\r\n";
  return oss.str();
}

}  // namespace nvecd::server::handlers
